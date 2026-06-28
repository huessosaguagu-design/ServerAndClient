#include "network.h"
#include "protocol.h"

#ifdef _WIN32
  #define WIN32_LEAN_AND_MEAN
  #include <winsock2.h>
  #include <ws2tcpip.h>
  #pragma comment(lib, "ws2_32.lib")
  using socklen_t = int;
#else
  #include <sys/socket.h>
  #include <netinet/in.h>
  #include <arpa/inet.h>
  #include <netdb.h>
  #include <unistd.h>
  #include <cerrno>
  #define closesocket close
  using SOCKET = int;
  static constexpr int INVALID_SOCKET = -1;
  #define SD_BOTH SHUT_RDWR
#endif

#include <cstring>

namespace net {

// ── init / shutdown ─────────────────────────────────────────────────
bool init() {
#ifdef _WIN32
    WSADATA wsa;
    return WSAStartup(MAKEWORD(2, 2), &wsa) == 0;
#else
    return true;
#endif
}

void shutdown() {
#ifdef _WIN32
    WSACleanup();
#endif
}

// ── connect ─────────────────────────────────────────────────────────
SocketT connect(const std::string& host, int port) {
    struct addrinfo hints {};
    hints.ai_family   = AF_INET;        // IPv4
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;

    addrinfo* result = nullptr;
    std::string portStr = std::to_string(port);
    if (getaddrinfo(host.c_str(), portStr.c_str(), &hints, &result) != 0)
        return INVALID;

    SocketT sock = INVALID;
    for (auto* ptr = result; ptr != nullptr; ptr = ptr->ai_next) {
        sock = static_cast<SocketT>(
            ::socket(ptr->ai_family, ptr->ai_socktype, ptr->ai_protocol));
        if (sock == INVALID) continue;

        if (::connect(sock, ptr->ai_addr, static_cast<int>(ptr->ai_addrlen)) == 0)
            break;  // success

        close(sock);
        sock = INVALID;
    }
    freeaddrinfo(result);
    return sock;
}

// ── sendAll / recvAll ───────────────────────────────────────────────
bool sendAll(SocketT sock, const void* data, size_t len) {
    auto* p = static_cast<const char*>(data);
    while (len > 0) {
        int n = ::send(static_cast<SOCKET>(sock), p, static_cast<int>(len), 0);
        if (n <= 0) return false;
        p   += n;
        len -= static_cast<size_t>(n);
    }
    return true;
}

bool recvAll(SocketT sock, void* data, size_t len) {
    auto* p = static_cast<char*>(data);
    while (len > 0) {
        int n = ::recv(static_cast<SOCKET>(sock), p, static_cast<int>(len), 0);
        if (n <= 0) return false;
        p   += n;
        len -= static_cast<size_t>(n);
    }
    return true;
}

// ── sendMessage ─────────────────────────────────────────────────────
bool sendMessage(SocketT sock, const std::vector<uint8_t>& msg) {
    return sendAll(sock, msg.data(), msg.size());
}

// ── recvMessage ────────────────────────────────────────────────────
bool recvMessage(SocketT sock, uint8_t& msgType, uint8_t& cmdType,
                 std::vector<uint8_t>& payload)
{
    // Read 6-byte header
    uint8_t hdr[proto::HEADER_SIZE];
    if (!recvAll(sock, hdr, proto::HEADER_SIZE))
        return false;

    uint32_t payloadSize;
    std::memcpy(&payloadSize, hdr, 4);
    msgType  = hdr[4];
    cmdType  = hdr[5];

    // Safety cap: 64 MB max payload
    if (payloadSize > 64 * 1024 * 1024)
        return false;

    payload.resize(payloadSize);
    if (payloadSize > 0 && !recvAll(sock, payload.data(), payloadSize))
        return false;

    return true;
}

// ── close ───────────────────────────────────────────────────────────
void close(SocketT sock) {
    if (sock != INVALID) {
        ::shutdown(static_cast<SOCKET>(sock), SD_BOTH);
        closesocket(static_cast<SOCKET>(sock));
    }
}

// ── Server ──────────────────────────────────────────────────────────
bool Server::start(int port) {
    listenSock_ = static_cast<SocketT>(
        ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP));
    if (listenSock_ == INVALID)
        return false;

    // Allow quick rebind
    int opt = 1;
    setsockopt(static_cast<SOCKET>(listenSock_), SOL_SOCKET,
              SO_REUSEADDR, reinterpret_cast<char*>(&opt), sizeof(opt));

    sockaddr_in addr{};
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr= INADDR_ANY;
    addr.sin_port       = htons(static_cast<u_short>(port));

    if (::bind(static_cast<SOCKET>(listenSock_),
               reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == SOCKET_ERROR) {
        closesocket(static_cast<SOCKET>(listenSock_));
        listenSock_ = INVALID;
        return false;
    }
    if (::listen(static_cast<SOCKET>(listenSock_), 8) == SOCKET_ERROR) {
        closesocket(static_cast<SOCKET>(listenSock_));
        listenSock_ = INVALID;
        return false;
    }
    return true;
}

SocketT Server::acceptClient() {
    sockaddr_in clientAddr{};
    socklen_t addrLen = sizeof(clientAddr);
    SOCKET c = ::accept(static_cast<SOCKET>(listenSock_),
                        reinterpret_cast<sockaddr*>(&clientAddr), &addrLen);
    if (c == INVALID_SOCKET)
        return INVALID;
    return static_cast<SocketT>(c);
}

void Server::stop() {
    if (listenSock_ != INVALID) {
        closesocket(static_cast<SOCKET>(listenSock_));
        listenSock_ = INVALID;
    }
}

Server::~Server() { stop(); }

// ── Receiver ────────────────────────────────────────────────────────
void Receiver::start(SocketT sock, OnMessage cb) {
    if (thread_.joinable())
        thread_.join();
    sock_ = sock;
    cb_   = std::move(cb);
    running_ = true;
    thread_ = std::thread([this]() {
        while (running_) {
            uint8_t msgType, cmdType;
            std::vector<uint8_t> payload;
            if (!recvMessage(sock_, msgType, cmdType, payload)) {
                running_ = false;
                if (cb_) cb_(0, 0, {});  // signal disconnect
                break;
            }
            if (cb_) cb_(msgType, cmdType, std::move(payload));
        }
    });
}

void Receiver::stop() {
    running_ = false;
    // Closing the socket unblocks recv()
    if (sock_ != INVALID) {
        ::shutdown(static_cast<SOCKET>(sock_), SD_BOTH);
    }
    if (thread_.joinable())
        thread_.join();
    // Don't double-close: the owner of the socket will close it.
}

} // namespace net

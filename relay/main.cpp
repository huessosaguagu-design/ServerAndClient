// ═══════════════════════════════════════════════════════════════════
//  RELAY SERVER — message router deployed on OnRender
//
//  Accepts TCP connections from clients and servers, routes messages
//  between them.  Also responds to HTTP health checks (OnRender requirement).
//  Reads PORT from environment (OnRender requirement).
// ═══════════════════════════════════════════════════════════════════
#include "network.h"
#include "protocol.h"

#ifdef _WIN32
  #include <winsock2.h>
  #include <ws2tcpip.h>
#else
  #include <sys/socket.h>
  #include <netinet/in.h>
  #include <unistd.h>
#endif

#include <iostream>
#include <map>
#include <vector>
#include <mutex>
#include <thread>
#include <atomic>
#include <cstdlib>
#include <cstring>

// ── Shared state ───────────────────────────────────────────────────
static std::mutex             g_mtx;
static std::map<uint32_t, net::SocketT> g_clients;   // id → socket
static std::vector<net::SocketT>        g_servers;
static std::map<net::SocketT, std::string> g_names;  // socket → name
static std::atomic<uint32_t>  g_nextId{1};

// ── Snapshot helpers (copy under lock, I/O outside lock) ──────────
static std::vector<net::SocketT> snapshotServers() {
    std::lock_guard<std::mutex> lk(g_mtx);
    return g_servers;
}

static net::SocketT findClient(uint32_t id) {
    std::lock_guard<std::mutex> lk(g_mtx);
    auto it = g_clients.find(id);
    return (it != g_clients.end()) ? it->second : net::INVALID;
}

static std::vector<uint8_t> buildClientListPayload() {
    std::lock_guard<std::mutex> lk(g_mtx);
    proto::Writer w;
    w.u32(static_cast<uint32_t>(g_clients.size()));
    for (auto& [id, sock] : g_clients) {
        w.u32(id);
        auto nit = g_names.find(sock);
        w.str(nit != g_names.end() ? nit->second : "unknown");
    }
    return w.bytes();
}

static void broadcastClientList() {
    auto payload = buildClientListPayload();
    auto msg = proto::buildMessage(proto::MSG_CLIENT_LIST, 0, payload);
    auto srvs = snapshotServers();
    for (auto s : srvs)
        net::sendAll(s, msg.data(), msg.size());
}

static void sendToAllServers(const std::vector<uint8_t>& msg) {
    auto srvs = snapshotServers();
    for (auto s : srvs)
        net::sendAll(s, msg.data(), msg.size());
}

// ── HTTP health check handler (OnRender pings with HTTP GET) ───────
static bool isHttpRequest(const uint8_t* data) {
    return memcmp(data, "GET", 3) == 0 || memcmp(data, "POS", 3) == 0 ||
           memcmp(data, "HEA", 3) == 0 || memcmp(data, "PUT", 3) == 0 ||
           memcmp(data, "DEL", 3) == 0 || memcmp(data, "OPT", 3) == 0;
}

static void handleHttpCheck(net::SocketT sock) {
    // Set short timeout and drain remaining HTTP data
#ifdef _WIN32
    DWORD timeout = 1000;
    setsockopt(static_cast<SOCKET>(sock), SOL_SOCKET, SO_RCVTIMEO,
               reinterpret_cast<char*>(&timeout), sizeof(timeout));
#else
    struct timeval tv = {1, 0};
    setsockopt(static_cast<SOCKET>(sock), SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
#endif
    char buf[4096];
    while (true) {
        int n = ::recv(static_cast<SOCKET>(sock), buf, sizeof(buf), 0);
        if (n <= 0) break;
    }
    static const char resp[] = "HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nOK";
    net::sendAll(sock, resp, sizeof(resp) - 1);
    net::close(sock);
}

// ── Connection handler ─────────────────────────────────────────────
static void handleConnection(net::SocketT sock) {
    // ── Read 6-byte header manually (to detect HTTP vs relay) ───────
    uint8_t hdr[proto::HEADER_SIZE];
    if (!net::recvAll(sock, hdr, proto::HEADER_SIZE)) {
        net::close(sock);
        return;
    }

    // HTTP health check?
    if (isHttpRequest(hdr)) {
        handleHttpCheck(sock);
        return;
    }

    // ── Parse relay header ─────────────────────────────────────────
    uint32_t payloadSize;
    memcpy(&payloadSize, hdr, 4);
    uint8_t msgType = hdr[4];
    uint8_t cmdType = hdr[5];

    if (payloadSize > 64 * 1024 * 1024) { net::close(sock); return; }

    std::vector<uint8_t> payload(payloadSize);
    if (payloadSize > 0 && !net::recvAll(sock, payload.data(), payloadSize)) {
        net::close(sock);
        return;
    }

    // ── Phase 1: REGISTER ──────────────────────────────────────────
    if (msgType != proto::MSG_REGISTER || payload.empty()) {
        net::close(sock);
        return;
    }

    uint8_t role = payload[0];

    if (role == proto::ROLE_CLIENT) {
        // ── Client registration ───────────────────────────────────
        std::string name;
        if (payload.size() > 1) {
            proto::Reader r(payload.data() + 1, payload.size() - 1);
            r.str(name);
        }
        if (name.empty()) name = "client";

        uint32_t id = g_nextId.fetch_add(1);
        {
            std::lock_guard<std::mutex> lk(g_mtx);
            g_clients[id]   = sock;
            g_names[sock]   = name;
        }

        // Send assigned ID back to client
        proto::Writer w;
        w.u32(id);
        auto ack = proto::buildMessage(proto::MSG_REGISTER, 0, w.bytes());
        net::sendAll(sock, ack.data(), ack.size());

        broadcastClientList();
        std::cout << "[+] Client \"" << name << "\" registered as id=" << id << "\n";

        // ── Client message loop ───────────────────────────────────
        while (true) {
            if (!net::recvMessage(sock, msgType, cmdType, payload))
                break;
            if (msgType == proto::MSG_RESPONSE) {
                proto::Writer w2;
                w2.u32(id);
                w2.raw(payload.data(), payload.size());
                auto fwd = proto::buildMessage(proto::MSG_RESPONSE, cmdType, w2.bytes());
                sendToAllServers(fwd);
            }
        }

        {
            std::lock_guard<std::mutex> lk(g_mtx);
            g_clients.erase(id);
            g_names.erase(sock);
        }
        net::close(sock);
        broadcastClientList();
        std::cout << "[-] Client id=" << id << " disconnected\n";

    } else if (role == proto::ROLE_SERVER) {
        // ── Server registration ───────────────────────────────────
        {
            std::lock_guard<std::mutex> lk(g_mtx);
            g_servers.push_back(sock);
        }

        {
            auto pl = buildClientListPayload();
            auto msg = proto::buildMessage(proto::MSG_CLIENT_LIST, 0, pl);
            net::sendAll(sock, msg.data(), msg.size());
        }
        std::cout << "[+] Server connected\n";

        // ── Server message loop ────────────────────────────────────
        while (true) {
            if (!net::recvMessage(sock, msgType, cmdType, payload))
                break;
            if (msgType == proto::MSG_COMMAND && payload.size() >= 4) {
                uint32_t targetId;
                memcpy(&targetId, payload.data(), 4);

                std::vector<uint8_t> fwdPayload(payload.begin() + 4, payload.end());
                auto fwd = proto::buildMessage(proto::MSG_COMMAND, cmdType, fwdPayload);

                net::SocketT cs = findClient(targetId);
                if (cs != net::INVALID)
                    net::sendAll(cs, fwd.data(), fwd.size());
            }
        }

        {
            std::lock_guard<std::mutex> lk(g_mtx);
            g_servers.erase(
                std::remove(g_servers.begin(), g_servers.end(), sock),
                g_servers.end());
        }
        net::close(sock);
        std::cout << "[-] Server disconnected\n";

    } else {
        net::close(sock);
    }
}

// ── main ───────────────────────────────────────────────────────────
int main(int argc, char** argv) {
    int port = 10000;

    if (const char* env = std::getenv("PORT"))
        port = std::atoi(env);
    if (argc > 1)
        port = std::atoi(argv[1]);

    if (!net::init()) {
        std::cerr << "FATAL: cannot init networking\n";
        return 1;
    }

    net::Server server;
    if (!server.start(port)) {
        std::cerr << "FATAL: cannot listen on port " << port << "\n";
        net::shutdown();
        return 1;
    }

    std::cout << "========================================\n"
              << "  Relay server running on port " << port << "\n"
              << "========================================\n";

    while (true) {
        net::SocketT client = server.acceptClient();
        if (client == net::INVALID) break;
        std::thread(handleConnection, client).detach();
    }

    server.stop();
    net::shutdown();
    return 0;
}

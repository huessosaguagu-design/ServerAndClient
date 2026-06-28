#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include <functional>
#include <mutex>
#include <atomic>
#include <thread>

// ═══════════════════════════════════════════════════════════════════
//  NETWORK — cross-platform TCP wrapper (WinSock2 / POSIX)
// ═══════════════════════════════════════════════════════════════════

namespace net {

using SocketT = int;
static constexpr SocketT INVALID = -1;

// ── Initialise / cleanup the networking subsystem ──────────────────
bool init();
void shutdown();

// ── Connect to host:port (blocking, returns INVALID on failure) ────
SocketT connect(const std::string& host, int port);

// ── Send exactly n bytes; returns false on failure ─────────────────
bool sendAll(SocketT sock, const void* data, size_t len);

// ── Receive exactly n bytes; returns false on failure / EOF ────────
bool recvAll(SocketT sock, void* data, size_t len);

// ── Send one framed message (header + payload) ─────────────────────
bool sendMessage(SocketT sock, const std::vector<uint8_t>& msg);

// ── Receive one framed message (blocking). Returns false on failure.─
//  Fills msgType, cmdType, and payload.
bool recvMessage(SocketT sock, uint8_t& msgType, uint8_t& cmdType,
                 std::vector<uint8_t>& payload);

// ── Close a socket ──────────────────────────────────────────────────
void close(SocketT sock);

// ── Server listener ────────────────────────────────────────────────
class Server {
public:
    bool start(int port);
    void stop();
    // Blocks until a new client connects; returns INVALID if stopped.
    SocketT acceptClient();
    ~Server();
private:
    SocketT listenSock_ = INVALID;
};

// ── Background receiver thread ─────────────────────────────────────
//  Spawns a thread that calls recvMessage in a loop and dispatches
//  to a callback.  Thread-safe.
class Receiver {
public:
    using OnMessage = std::function<void(uint8_t msgType, uint8_t cmdType,
                                         std::vector<uint8_t> payload)>;
    void start(SocketT sock, OnMessage cb);
    void stop();
    bool running() const { return running_; }
    ~Receiver() { stop(); }
private:
    std::thread thread_;
    std::atomic<bool> running_{false};
    SocketT sock_ = INVALID;
    OnMessage cb_;
};

} // namespace net

#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include <functional>
#include <mutex>
#include <atomic>
#include <thread>

// ═══════════════════════════════════════════════════════════════════
//  WSWIN — WebSocket client using WinHTTP (WSS for OnRender)
//
//  Same interface as net::, but uses WebSocket protocol.
//  Port 443 → WSS (TLS), other ports → WS (plain, for local testing).
// ═══════════════════════════════════════════════════════════════════

struct WsSession;

namespace ws {

using SocketT = WsSession*;
static constexpr SocketT INVALID = nullptr;

// Init / shutdown WinHTTP
bool init();
void shutdown();

// Last error string (human-readable)
std::string lastError();

// Connect to wss://host:port/path (or ws:// if port != 443)
SocketT connect(const std::string& host, int port, const std::string& path = "/ws");

// Send exactly n bytes as one WebSocket binary message
bool sendAll(SocketT sock, const void* data, size_t len);

// Receive exactly n bytes (buffered internally)
bool recvAll(SocketT sock, void* data, size_t len);

// Send one framed message (header + payload)
bool sendMessage(SocketT sock, const std::vector<uint8_t>& msg);

// Receive one framed message (blocking)
bool recvMessage(SocketT sock, uint8_t& msgType, uint8_t& cmdType,
                 std::vector<uint8_t>& payload);

// Close WebSocket session
void close(SocketT sock);

// Background receiver thread
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

} // namespace ws

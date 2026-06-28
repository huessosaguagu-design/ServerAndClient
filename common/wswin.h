#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include <functional>
#include <mutex>
#include <atomic>
#include <thread>

// ═══════════════════════════════════════════════════════════════════
//  HTTPPOLL — HTTP long-poll transport (works through any proxy)
//
//  Instead of WebSocket, uses simple HTTP:
//    POST /register → get session ID
//    POST /send?id=X → send binary message
//    GET /poll?id=X → long-poll for incoming messages
// ═══════════════════════════════════════════════════════════════════

struct HttpPollSession;

namespace ws {

using SocketT = HttpPollSession*;
static constexpr SocketT INVALID = nullptr;

bool init();
void shutdown();
std::string lastError();

// Connect: POST /register with payload, returns session
SocketT connect(const std::string& host, int port, const std::string& path = "/register");

// Send a binary message via POST /send
bool sendAll(SocketT sock, const void* data, size_t len);

// Not used in HTTP mode — kept for compatibility
bool recvAll(SocketT sock, void* data, size_t len);
bool sendMessage(SocketT sock, const std::vector<uint8_t>& msg);
bool recvMessage(SocketT sock, uint8_t& msgType, uint8_t& cmdType,
                 std::vector<uint8_t>& payload);

void close(SocketT sock);

// Background poller thread
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

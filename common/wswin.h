#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include <functional>
#include <mutex>
#include <atomic>
#include <thread>

// ═══════════════════════════════════════════════════════════════════
//  WSWIN — WebSocket client using WinHTTP (WSS for Railway)
// ═══════════════════════════════════════════════════════════════════

struct WsSession;

namespace ws {

using SocketT = WsSession*;
static constexpr SocketT INVALID = nullptr;

bool init();
void shutdown();
std::string lastError();

// Connect to wss://host:port/path
SocketT connect(const std::string& host, int port, const std::string& path = "/ws");

bool sendAll(SocketT sock, const void* data, size_t len);
bool recvAll(SocketT sock, void* data, size_t len);
bool sendMessage(SocketT sock, const std::vector<uint8_t>& msg);
bool recvMessage(SocketT sock, uint8_t& msgType, uint8_t& cmdType,
                 std::vector<uint8_t>& payload);

void close(SocketT sock);

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

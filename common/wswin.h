#pragma once
#include <cstdint>
#include <cstddef>
#include <string>
#include <vector>
#include <functional>
#include <mutex>
#include <atomic>
#include <thread>

struct HttpPollSession;

namespace ws {

using SocketT = HttpPollSession*;
static constexpr SocketT INVALID = nullptr;

bool init();
void shutdown();
std::string lastError();

// Connect: POST /register with payload, returns session
SocketT connect(const std::string& host, int port, const std::string& regPayload);

bool sendAll(SocketT sock, const void* data, size_t len);
bool sendMessage(SocketT sock, const std::vector<uint8_t>& msg);

// Async send — enqueues data to a background thread, never blocks the UI.
// Use for commands that don't need a synchronous ack (draw, mouse, keys).
void startAsyncSend(SocketT sock);
void stopAsyncSend();
bool sendAllAsync(SocketT sock, const void* data, size_t len);

inline bool recvAll(SocketT, void*, size_t) { return false; }
inline bool recvMessage(SocketT, uint8_t&, uint8_t&, std::vector<uint8_t>&) { return false; }

void close(SocketT sock);

class Receiver {
public:
    using OnMessage = std::function<void(uint8_t msgType, uint8_t cmdType,
                                         std::vector<uint8_t> payload)>;
    // The optional `aliveFlag` is checked after every callback; the receiver
    // loop exits cleanly when the consumer sets it to false (e.g. on
    // MSG_DISCONNECT or MSG_KICK_CLIENT). Returning a flag means we never
    // emit a synthetic disconnect on transient network blips.
    void start(SocketT sock, OnMessage cb, bool* aliveFlag = 0);
    void stop();
    bool running() const { return running_; }
    ~Receiver() { stop(); }
private:
    std::thread thread_;
    std::atomic<bool> running_{false};
    SocketT sock_ = INVALID;
    OnMessage cb_;
    bool* aliveFlag_ = nullptr;
};

} // namespace ws

#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include <cstring>

// ═══════════════════════════════════════════════════════════════════
//  PROTOCOL — binary message framing used by server, client, and relay
// ═══════════════════════════════════════════════════════════════════
//
//  Wire format (little-endian):
//
//  ┌──────────┬──────────┬──────────┬────────────────────────┐
//  │ payloadSz│  msgType │  cmdType │  payload bytes ...     │
//  │  uint32  │  uint8   │  uint8   │  (payloadSz bytes)     │
//  └──────────┴──────────┴──────────┴────────────────────────┘
//
//  Header is 6 bytes.  payloadSize does NOT include the header.

namespace proto {

// ── Message types ──────────────────────────────────────────────────
enum MsgType : uint8_t {
    MSG_REGISTER     = 1,   // role + name → relay
    MSG_CLIENT_LIST  = 2,   // relay → server: list of connected clients
    MSG_COMMAND      = 3,   // server → client (relayed)
    MSG_RESPONSE     = 4,   // client → server (relayed)
    MSG_HEARTBEAT    = 5,
    MSG_DISCONNECT   = 6,
};

// ── Roles for REGISTER ─────────────────────────────────────────────
enum Role : uint8_t {
    ROLE_CLIENT = 1,
    ROLE_SERVER = 2,
};

// ── Command types (carried in cmdType for COMMAND/RESPONSE) ────────
enum CmdType : uint8_t {
    CMD_BROWSE_FILES  = 1,
    CMD_SYSTEM_INFO   = 2,
    CMD_VIEW_SCREEN   = 3,
    CMD_EXECUTE       = 4,
    CMD_MOUSE_CLICK   = 5,
    CMD_KEY_INPUT     = 6,
    CMD_FILE_UPLOAD   = 7,
    CMD_RUN_FILE      = 8,
    CMD_PROCESS_LIST  = 9,
    CMD_KILL_PROCESS  = 10,
};

// ── Header ─────────────────────────────────────────────────────────
struct Header {
    uint32_t payloadSize;
    uint8_t  msgType;
    uint8_t  cmdType;
};
static constexpr uint32_t HEADER_SIZE = 6;

// ── Helpers to build / parse payloads as simple byte buffers ───────
//  Payloads use a tiny TLV-free scheme: just concatenated fields with
//  length prefixes for variable-length strings.

class Writer {
public:
    void u32(uint32_t v) { append(&v, 4); }
    void u64(uint64_t v) { append(&v, 8); }
    void u8(uint8_t v)   { append(&v, 1); }
    void str(const std::string& s) {
        u32(static_cast<uint32_t>(s.size()));
        append(s.data(), s.size());
    }
    void raw(const void* data, size_t len) {
        append(data, len);
    }
    const std::vector<uint8_t>& bytes() const { return buf_; }
    size_t size() const { return buf_.size(); }

private:
    std::vector<uint8_t> buf_;
    void append(const void* src, size_t n) {
        auto p = static_cast<const uint8_t*>(src);
        buf_.insert(buf_.end(), p, p + n);
    }
};

class Reader {
public:
    Reader(const uint8_t* data, size_t len) : p_(data), end_(data + len) {}
    bool u32(uint32_t& out) { return read(&out, 4); }
    bool u64(uint64_t& out) { return read(&out, 8); }
    bool u8(uint8_t& out)   { return read(&out, 1); }
    bool str(std::string& out) {
        uint32_t len;
        if (!u32(len)) return false;
        if (p_ + len > end_) return false;
        out.assign(reinterpret_cast<const char*>(p_), len);
        p_ += len;
        return true;
    }
    bool raw(std::vector<uint8_t>& out, size_t len) {
        if (p_ + len > end_) return false;
        out.assign(p_, p_ + len);
        p_ += len;
        return true;
    }
    size_t remaining() const { return static_cast<size_t>(end_ - p_); }
private:
    const uint8_t* p_;
    const uint8_t* end_;
    bool read(void* dst, size_t n) {
        if (p_ + n > end_) return false;
        std::memcpy(dst, p_, n);
        p_ += n;
        return true;
    }
};

// ── Full message: header + payload, ready to send on the wire ──────
inline std::vector<uint8_t> buildMessage(uint8_t msgType, uint8_t cmdType,
                                         const uint8_t* payload, uint32_t payloadSize)
{
    std::vector<uint8_t> msg;
    msg.resize(HEADER_SIZE + payloadSize);
    std::memcpy(msg.data() + 0, &payloadSize, 4);
    msg[4] = msgType;
    msg[5] = cmdType;
    if (payloadSize > 0)
        std::memcpy(msg.data() + HEADER_SIZE, payload, payloadSize);
    return msg;
}

inline std::vector<uint8_t> buildMessage(uint8_t msgType, uint8_t cmdType,
                                        const std::vector<uint8_t>& payload)
{
    return buildMessage(msgType, cmdType, payload.data(),
                        static_cast<uint32_t>(payload.size()));
}

} // namespace proto

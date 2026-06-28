#include "wswin.h"
#include "protocol.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winhttp.h>
#pragma comment(lib, "winhttp.lib")

#include <cstring>
#include <cstdio>
#include <cstdarg>
#include <cstdlib>

// ═══════════════════════════════════════════════════════════════════
//  WsSession — per-connection state with receive buffer
// ═══════════════════════════════════════════════════════════════════
struct WsSession {
    HINTERNET hSession  = nullptr;
    HINTERNET hConnect  = nullptr;
    HINTERNET hSocket   = nullptr;  // WebSocket handle

    // Receive buffer — WebSocket frames may not align with message boundaries
    std::vector<uint8_t> recvBuf;
    size_t readPos = 0;

    ~WsSession() {
        if (hSocket)  WinHttpCloseHandle(hSocket);
        if (hConnect) WinHttpCloseHandle(hConnect);
        if (hSession) WinHttpCloseHandle(hSession);
    }

    // Fill buffer with data from a WebSocket frame
    bool fillBuffer() {
        uint8_t buffer[65536];
        DWORD dwBytesRead = 0;
        WINHTTP_WEB_SOCKET_BUFFER_TYPE bufferType = {};

        DWORD result = WinHttpWebSocketReceive(hSocket, buffer, sizeof(buffer), &dwBytesRead, &bufferType);
        if (result != ERROR_SUCCESS)
            return false;

        // Check frame type
        if (bufferType == WINHTTP_WEB_SOCKET_CLOSE_BUFFER_TYPE)
            return false;  // Connection closing

        // Binary data — store in buffer
        recvBuf.assign(buffer, buffer + dwBytesRead);
        readPos = 0;
        return true;
    }
};

namespace ws {

// ── Error storage ─────────────────────────────────────────────────
static std::string g_lastError;

std::string lastError() { return g_lastError; }

static void setError(const char* fmt, ...) {
    char buf[256];
    va_list args; va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    g_lastError = buf;
}

static const char* winHttpErrorStr(DWORD err) {
    switch (err) {
        case 12002: return "Timeout";
        case 12007: return "DNS not resolved";
        case 12029: return "Connection refused";
        case 12031: return "Connection reset";
        case 12057: return "Secure channel error";
        case 12157: return "SSL error";
        case 12169: return "Invalid certificate";
        case 12175: return "Cert validation failed";
        default: return "Unknown";
    }
}

// ── init / shutdown ───────────────────────────────────────────────
bool init() { g_lastError.clear(); return true; }
void shutdown() {}

// ── connect ───────────────────────────────────────────────────────
SocketT connect(const std::string& host, int port, const std::string& path) {

    // Convert to wide strings (once, outside retry loop)
    int wlen = MultiByteToWideChar(CP_UTF8, 0, host.c_str(), -1, nullptr, 0);
    std::wstring whost(wlen, 0);
    MultiByteToWideChar(CP_UTF8, 0, host.c_str(), -1, &whost[0], wlen);
    whost.resize(wlen - 1);

    wlen = MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, nullptr, 0);
    std::wstring wpath(wlen, 0);
    MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, &wpath[0], wlen);
    wpath.resize(wlen - 1);

    // Retry loop — OnRender free plan may be sleeping and need time to wake
    for (int attempt = 0; attempt < 3; attempt++) {
        WsSession* s = new WsSession();

        // 1. Create WinHTTP session
        s->hSession = WinHttpOpen(L"RemoteControl/5.0",
            WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
            WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
        if (!s->hSession) { delete s; continue; }

        // Set long timeouts — OnRender free plan takes 30-60s to wake from sleep
        WinHttpSetTimeouts(s->hSession,
            90000,   // resolve timeout: 90s
            90000,   // connect timeout: 90s
            90000,   // send timeout: 90s
            90000);  // receive timeout: 90s

        // 2. Connect to server
        s->hConnect = WinHttpConnect(s->hSession, whost.c_str(),
            (port == 443) ? INTERNET_DEFAULT_HTTPS_PORT : port, 0);
        if (!s->hConnect) { delete s; continue; }

        // 3. Create request with WebSocket upgrade flag
        DWORD flags = (port == 443) ? WINHTTP_FLAG_SECURE : 0;
        HINTERNET hRequest = WinHttpOpenRequest(s->hConnect, L"GET", wpath.c_str(),
            nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, flags);
        if (!hRequest) { delete s; continue; }

        // 4. Enable WebSocket upgrade
        BOOL upgrade = TRUE;
        WinHttpSetOption(hRequest, WINHTTP_OPTION_UPGRADE_TO_WEB_SOCKET,
            &upgrade, sizeof(upgrade));

        // 5. Send request
        if (!WinHttpSendRequest(hRequest, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
            WINHTTP_NO_REQUEST_DATA, 0, 0, 0)) {
            DWORD err = GetLastError();
            setError("Send failed: %s (err=%lu)", winHttpErrorStr(err), err);
            WinHttpCloseHandle(hRequest);
            delete s;
            if (attempt < 2) { Sleep(3000); }
            continue;
        }

        // 6. Receive response (should be 101 Switching Protocols)
        if (!WinHttpReceiveResponse(hRequest, nullptr)) {
            DWORD err = GetLastError();
            setError("Receive failed: %s (err=%lu)", winHttpErrorStr(err), err);
            WinHttpCloseHandle(hRequest);
            delete s;
            if (attempt < 2) { Sleep(3000); }
            continue;
        }

        // Check HTTP status code
        DWORD statusCode = 0;
        DWORD statusCodeSize = sizeof(statusCode);
        WinHttpQueryHeaders(hRequest, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
            WINHTTP_HEADER_NAME_BY_INDEX, &statusCode, &statusCodeSize, WINHTTP_NO_HEADER_INDEX);
        if (statusCode != 101) {
            setError("Server returned HTTP %lu (expected 101). "
                     "Check: 1) relay is Python+WebSocket  2) path is /ws", statusCode);
            WinHttpCloseHandle(hRequest);
            delete s;
            if (attempt < 2) { Sleep(3000); }
            continue;
        }

        // 7. Complete WebSocket upgrade
        s->hSocket = WinHttpWebSocketCompleteUpgrade(hRequest, 0);
        WinHttpCloseHandle(hRequest);

        if (!s->hSocket) {
            DWORD err = GetLastError();
            setError("WebSocket upgrade failed: %s (err=%lu)", winHttpErrorStr(err), err);
            delete s;
            if (attempt < 2) { Sleep(3000); }
            continue;
        }

        setError("OK");
        return s;  // Success!
    }

    return INVALID;  // All retries failed
}

// ── sendAll ───────────────────────────────────────────────────────
bool sendAll(SocketT sock, const void* data, size_t len) {
    if (!sock || !sock->hSocket) return false;

    DWORD result = WinHttpWebSocketSend(sock->hSocket,
        WINHTTP_WEB_SOCKET_BINARY_MESSAGE_BUFFER_TYPE,
        (PVOID)data, (DWORD)len);
    return result == ERROR_SUCCESS;
}

// ── recvAll ───────────────────────────────────────────────────────
bool recvAll(SocketT sock, void* data, size_t len) {
    if (!sock || !sock->hSocket) return false;
    auto* p = static_cast<uint8_t*>(data);

    while (len > 0) {
        // If buffer is empty, fill it
        if (sock->readPos >= sock->recvBuf.size()) {
            if (!sock->fillBuffer())
                return false;
        }

        // Copy from buffer
        size_t available = sock->recvBuf.size() - sock->readPos;
        size_t toCopy = (len < available) ? len : available;
        memcpy(p, sock->recvBuf.data() + sock->readPos, toCopy);
        p += toCopy;
        sock->readPos += toCopy;
        len -= toCopy;
    }

    return true;
}

// ── sendMessage ───────────────────────────────────────────────────
bool sendMessage(SocketT sock, const std::vector<uint8_t>& msg) {
    return sendAll(sock, msg.data(), msg.size());
}

// ── recvMessage ────────────────────────────────────────────────────
bool recvMessage(SocketT sock, uint8_t& msgType, uint8_t& cmdType,
                 std::vector<uint8_t>& payload)
{
    uint8_t hdr[proto::HEADER_SIZE];
    if (!recvAll(sock, hdr, proto::HEADER_SIZE))
        return false;

    uint32_t payloadSize;
    memcpy(&payloadSize, hdr, 4);
    msgType = hdr[4];
    cmdType = hdr[5];

    if (payloadSize > 64 * 1024 * 1024)
        return false;

    payload.resize(payloadSize);
    if (payloadSize > 0 && !recvAll(sock, payload.data(), payloadSize))
        return false;

    return true;
}

// ── close ─────────────────────────────────────────────────────────
void close(SocketT sock) {
    if (!sock) return;
    if (sock->hSocket) {
        WinHttpWebSocketClose(sock->hSocket,
            WINHTTP_WEB_SOCKET_SUCCESS_CLOSE_STATUS, nullptr, 0);
    }
    delete sock;
}

// ── Receiver ───────────────────────────────────────────────────────
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
                if (cb_) cb_(0, 0, {});
                break;
            }
            if (cb_) cb_(msgType, cmdType, std::move(payload));
        }
    });
}

void Receiver::stop() {
    running_ = false;
    // WinHTTP WebSocket receive doesn't unblock with shutdown,
    // so we close the socket to force the recv to fail
    if (sock_ && sock_->hSocket) {
        WinHttpWebSocketClose(sock_->hSocket,
            WINHTTP_WEB_SOCKET_SUCCESS_CLOSE_STATUS, nullptr, 0);
    }
    if (thread_.joinable())
        thread_.join();
}

} // namespace ws

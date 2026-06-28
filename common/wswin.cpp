#include "wswin.h"
#include "protocol.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winhttp.h>
#pragma comment(lib, "winhttp.lib")

#include <cstring>
#include <cstdio>
#include <cstdarg>

// ═══════════════════════════════════════════════════════════════════
//  HttpPollSession — holds connection info + session ID
// ═══════════════════════════════════════════════════════════════════
struct HttpPollSession {
    std::string host;
    int port = 443;
    bool tls = true;
    std::wstring whost;
    std::string sessionId;
    HINTERNET hSession = nullptr;

    ~HttpPollSession() {
        if (hSession) WinHttpCloseHandle(hSession);
    }
};

namespace ws {

static std::string g_lastError;

std::string lastError() { return g_lastError; }

static void setError(const char* fmt, ...) {
    char buf[256];
    va_list args; va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    g_lastError = buf;
}

// ── HTTP helpers ──────────────────────────────────────────────────
static HINTERNET createSession() {
    return WinHttpOpen(L"RemoteControl/5.0",
        WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
        WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
}

static bool httpPost(HINTERNET hSession, const std::wstring& host, int port, bool tls,
                     const char* path, const void* body, size_t bodyLen,
                     std::string& response) {
    HINTERNET hConnect = WinHttpConnect(hSession, host.c_str(),
        (port == 443) ? INTERNET_DEFAULT_HTTPS_PORT : port, 0);
    if (!hConnect) return false;

    int wlen = MultiByteToWideChar(CP_UTF8, 0, path, -1, nullptr, 0);
    std::wstring wpath(wlen, 0);
    MultiByteToWideChar(CP_UTF8, 0, path, -1, &wpath[0], wlen);
    wpath.resize(wlen - 1);

    DWORD flags = tls ? WINHTTP_FLAG_SECURE : 0;
    HINTERNET hRequest = WinHttpOpenRequest(hConnect, L"POST", wpath.c_str(),
        nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, flags);
    if (!hRequest) { WinHttpCloseHandle(hConnect); return false; }

    WinHttpSetTimeouts(hRequest, 90000, 90000, 90000, 90000);

    // Content-Type header
    WinHttpAddRequestHeaders(hRequest, L"Content-Type: application/octet-stream",
        (DWORD)-1, WINHTTP_ADDREQ_FLAG_ADD);

    if (!WinHttpSendRequest(hRequest, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
        (void*)body, (DWORD)bodyLen, (DWORD)bodyLen, 0)) {
        WinHttpCloseHandle(hRequest); WinHttpCloseHandle(hConnect); return false;
    }

    if (!WinHttpReceiveResponse(hRequest, nullptr)) {
        WinHttpCloseHandle(hRequest); WinHttpCloseHandle(hConnect); return false;
    }

    // Read response
    DWORD bytesRead = 0;
    char buf[4096];
    response.clear();
    while (WinHttpReadData(hRequest, buf, sizeof(buf), &bytesRead) && bytesRead > 0)
        response.append(buf, bytesRead);

    WinHttpCloseHandle(hRequest);
    WinHttpCloseHandle(hConnect);
    return true;
}

static bool httpGetBinary(HINTERNET hSession, const std::wstring& host, int port, bool tls,
                          const char* path, std::vector<uint8_t>& response) {
    HINTERNET hConnect = WinHttpConnect(hSession, host.c_str(),
        (port == 443) ? INTERNET_DEFAULT_HTTPS_PORT : port, 0);
    if (!hConnect) return false;

    int wlen = MultiByteToWideChar(CP_UTF8, 0, path, -1, nullptr, 0);
    std::wstring wpath(wlen, 0);
    MultiByteToWideChar(CP_UTF8, 0, path, -1, &wpath[0], wlen);
    wpath.resize(wlen - 1);

    DWORD flags = tls ? WINHTTP_FLAG_SECURE : 0;
    HINTERNET hRequest = WinHttpOpenRequest(hConnect, L"GET", wpath.c_str(),
        nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, flags);
    if (!hRequest) { WinHttpCloseHandle(hConnect); return false; }

    // 35 second timeout for long-poll
    WinHttpSetTimeouts(hRequest, 90000, 90000, 90000, 40000);

    if (!WinHttpSendRequest(hRequest, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
        WINHTTP_NO_REQUEST_DATA, 0, 0, 0)) {
        WinHttpCloseHandle(hRequest); WinHttpCloseHandle(hConnect); return false;
    }

    if (!WinHttpReceiveResponse(hRequest, nullptr)) {
        WinHttpCloseHandle(hRequest); WinHttpCloseHandle(hConnect); return false;
    }

    // Check status code
    DWORD statusCode = 0;
    DWORD statusCodeSize = sizeof(statusCode);
    WinHttpQueryHeaders(hRequest, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
        WINHTTP_HEADER_NAME_BY_INDEX, &statusCode, &statusCodeSize, WINHTTP_NO_HEADER_INDEX);

    if (statusCode == 204) {
        // No content = no messages (long-poll timeout)
        WinHttpCloseHandle(hRequest); WinHttpCloseHandle(hConnect);
        response.clear();
        return true;
    }

    DWORD bytesRead = 0;
    uint8_t buf[65536];
    response.clear();
    while (WinHttpReadData(hRequest, buf, sizeof(buf), &bytesRead) && bytesRead > 0)
        response.insert(response.end(), buf, buf + bytesRead);

    WinHttpCloseHandle(hRequest);
    WinHttpCloseHandle(hConnect);
    return statusCode == 200;
}

// ── init / shutdown ───────────────────────────────────────────────
bool init() { g_lastError.clear(); return true; }
void shutdown() {}

// ── connect (POST /register) ──────────────────────────────────────
SocketT connect(const std::string& host, int port, const std::string& regPayload) {
    for (int attempt = 0; attempt < 3; attempt++) {
        HttpPollSession* s = new HttpPollSession();
        s->host = host;
        s->port = port;
        s->tls = (port == 443);

        int wlen = MultiByteToWideChar(CP_UTF8, 0, host.c_str(), -1, nullptr, 0);
        s->whost.resize(wlen);
        MultiByteToWideChar(CP_UTF8, 0, host.c_str(), -1, &s->whost[0], wlen);
        s->whost.resize(wlen - 1);

        s->hSession = createSession();
        if (!s->hSession) { delete s; continue; }

        WinHttpSetTimeouts(s->hSession, 90000, 90000, 90000, 90000);

        // POST /register with the register payload as body
        std::string resp;
        bool ok = httpPost(s->hSession, s->whost, s->port, s->tls,
                           "/register", regPayload.data(), regPayload.size(), resp);

        if (!ok || resp.empty()) {
            setError("Register failed (attempt %d)", attempt + 1);
            delete s;
            if (attempt < 2) Sleep(2000);
            continue;
        }

        // Response body = session ID
        s->sessionId = resp;
        setError("OK");
        return s;
    }

    if (g_lastError.empty()) setError("All retries failed");
    return INVALID;
}

// ── sendAll (POST /send?id=XXX) ───────────────────────────────────
bool sendAll(SocketT sock, const void* data, size_t len) {
    if (!sock) return false;

    char path[256];
    snprintf(path, sizeof(path), "/send?id=%s", sock->sessionId.c_str());

    std::string resp;
    return httpPost(sock->hSession, sock->whost, sock->port, sock->tls,
                    path, data, len, resp);
}

bool sendMessage(SocketT sock, const std::vector<uint8_t>& msg) {
    return sendAll(sock, msg.data(), msg.size());
}

// ── recvAll / recvMessage — not used (polling is in Receiver) ──────
bool recvAll(SocketT, void*, size_t) { return false; }
bool recvMessage(SocketT, uint8_t&, uint8_t&, std::vector<uint8_t>&) { return false; }

// ── close ─────────────────────────────────────────────────────────
void close(SocketT sock) {
    if (!sock) return;
    // Notify server of disconnect
    char path[256];
    snprintf(path, sizeof(path), "/disconnect?id=%s", sock->sessionId.c_str());
    std::string resp;
    httpPost(sock->hSession, sock->whost, sock->port, sock->tls, path, "", 0, resp);
    delete sock;
}

// ── Receiver — background long-poll loop ───────────────────────────
void Receiver::start(SocketT sock, OnMessage cb) {
    if (thread_.joinable())
        thread_.join();
    sock_ = sock;
    cb_   = std::move(cb);
    running_ = true;
    thread_ = std::thread([this]() {
        while (running_) {
            char path[256];
            snprintf(path, sizeof(path), "/poll?id=%s", sock_->sessionId.c_str());

            std::vector<uint8_t> resp;
            bool ok = httpGetBinary(sock_->hSession, sock_->whost, sock_->port,
                                    sock_->tls, path, resp);

            if (!running_) break;

            if (!ok) {
                // Network error — wait and retry
                Sleep(1000);
                continue;
            }

            if (resp.empty()) {
                // 204 = no messages, keep polling
                continue;
            }

            // Parse all messages from the response
            // Response may contain one or more framed messages
            size_t offset = 0;
            while (offset + proto::HEADER_SIZE <= resp.size()) {
                uint32_t payloadSize;
                memcpy(&payloadSize, resp.data() + offset, 4);
                uint8_t msgType = resp[offset + 4];
                uint8_t cmdType = resp[offset + 5];

                if (offset + proto::HEADER_SIZE + payloadSize > resp.size())
                    break;

                std::vector<uint8_t> payload(resp.data() + offset + proto::HEADER_SIZE,
                                             resp.data() + offset + proto::HEADER_SIZE + payloadSize);

                if (cb_) cb_(msgType, cmdType, std::move(payload));

                offset += proto::HEADER_SIZE + payloadSize;
            }
        }
        if (cb_) cb_(0, 0, {});  // signal disconnect
    });
}

void Receiver::stop() {
    running_ = false;
    if (thread_.joinable())
        thread_.join();
}

} // namespace ws

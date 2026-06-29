#include "wswin.h"
#include "protocol.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winhttp.h>
#pragma comment(lib, "winhttp.lib")

#include <cstring>
#include <cstdio>
#include <cstdarg>
#include <queue>
#include <condition_variable>

struct HttpPollSession {
    std::wstring whost;
    int port = 443;
    bool tls = true;
    std::string sessionId;
    HINTERNET hSession = nullptr;
    ~HttpPollSession() { if (hSession) WinHttpCloseHandle(hSession); }
};

namespace ws {

static std::string g_lastError;
std::string lastError() { return g_lastError; }

static void setError(const char* fmt, ...) {
    char buf[256]; va_list a; va_start(a, fmt);
    vsnprintf(buf, sizeof(buf), fmt, a); va_end(a);
    g_lastError = buf;
}

static bool httpPost(HINTERNET hSession, const std::wstring& host, int port, bool tls,
                     const char* path, const void* body, size_t bodyLen, std::string& resp) {
    HINTERNET hConn = WinHttpConnect(hSession, host.c_str(),
        (port == 443) ? INTERNET_DEFAULT_HTTPS_PORT : port, 0);
    if (!hConn) return false;

    int wl = MultiByteToWideChar(CP_UTF8, 0, path, -1, nullptr, 0);
    std::wstring wp(wl, 0);
    MultiByteToWideChar(CP_UTF8, 0, path, -1, &wp[0], wl);
    wp.resize(wl - 1);

    DWORD flags = tls ? WINHTTP_FLAG_SECURE : 0;
    HINTERNET hReq = WinHttpOpenRequest(hConn, L"POST", wp.c_str(), nullptr,
        WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, flags);
    if (!hReq) { WinHttpCloseHandle(hConn); return false; }

    WinHttpSetTimeouts(hReq, 15000, 15000, 15000, 15000);
    WinHttpAddRequestHeaders(hReq, L"Content-Type: application/octet-stream", -1, WINHTTP_ADDREQ_FLAG_ADD);

    if (!WinHttpSendRequest(hReq, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
        (void*)body, (DWORD)bodyLen, (DWORD)bodyLen, 0)) {
        WinHttpCloseHandle(hReq); WinHttpCloseHandle(hConn); return false;
    }
    if (!WinHttpReceiveResponse(hReq, nullptr)) {
        WinHttpCloseHandle(hReq); WinHttpCloseHandle(hConn); return false;
    }

    DWORD sc = 0, scs = sizeof(sc);
    WinHttpQueryHeaders(hReq, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
        WINHTTP_HEADER_NAME_BY_INDEX, &sc, &scs, WINHTTP_NO_HEADER_INDEX);
    if (sc != 200) {
        setError("HTTP %lu on POST %s", sc, path);
        WinHttpCloseHandle(hReq); WinHttpCloseHandle(hConn); return false;
    }

    DWORD br = 0; char buf[8192]; resp.clear();
    while (WinHttpReadData(hReq, buf, sizeof(buf), &br) && br > 0)
        resp.append(buf, br);
    while (!resp.empty() && (resp.back() == '\r' || resp.back() == '\n' || resp.back() == ' '))
        resp.pop_back();

    WinHttpCloseHandle(hReq); WinHttpCloseHandle(hConn);
    return true;
}

static bool httpGetBin(HINTERNET hSession, const std::wstring& host, int port, bool tls,
                       const char* path, std::vector<uint8_t>& resp) {
    HINTERNET hConn = WinHttpConnect(hSession, host.c_str(),
        (port == 443) ? INTERNET_DEFAULT_HTTPS_PORT : port, 0);
    if (!hConn) return false;

    int wl = MultiByteToWideChar(CP_UTF8, 0, path, -1, nullptr, 0);
    std::wstring wp(wl, 0);
    MultiByteToWideChar(CP_UTF8, 0, path, -1, &wp[0], wl);
    wp.resize(wl - 1);

    DWORD flags = tls ? WINHTTP_FLAG_SECURE : 0;
    HINTERNET hReq = WinHttpOpenRequest(hConn, L"GET", wp.c_str(), nullptr,
        WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, flags);
    if (!hReq) { WinHttpCloseHandle(hConn); return false; }

    WinHttpSetTimeouts(hReq, 30000, 30000, 30000, 30000);

    if (!WinHttpSendRequest(hReq, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0)) {
        WinHttpCloseHandle(hReq); WinHttpCloseHandle(hConn); return false;
    }
    if (!WinHttpReceiveResponse(hReq, nullptr)) {
        WinHttpCloseHandle(hReq); WinHttpCloseHandle(hConn); return false;
    }

    DWORD sc = 0, scs = sizeof(sc);
    WinHttpQueryHeaders(hReq, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
        WINHTTP_HEADER_NAME_BY_INDEX, &sc, &scs, WINHTTP_NO_HEADER_INDEX);

    if (sc == 204) { WinHttpCloseHandle(hReq); WinHttpCloseHandle(hConn); resp.clear(); return true; }
    if (sc != 200) { WinHttpCloseHandle(hReq); WinHttpCloseHandle(hConn); return false; }

    DWORD br = 0; uint8_t buf[262144]; resp.clear();
    while (WinHttpReadData(hReq, buf, sizeof(buf), &br) && br > 0)
        resp.insert(resp.end(), buf, buf + br);

    WinHttpCloseHandle(hReq); WinHttpCloseHandle(hConn);
    return true;
}

bool init() { g_lastError.clear(); return true; }
void shutdown() {}

SocketT connect(const std::string& host, int port, const std::string& regPayload) {
    for (int attempt = 0; attempt < 3; attempt++) {
        HttpPollSession* s = new HttpPollSession();
        s->port = port; s->tls = (port == 443);

        int wl = MultiByteToWideChar(CP_UTF8, 0, host.c_str(), -1, nullptr, 0);
        s->whost.resize(wl);
        MultiByteToWideChar(CP_UTF8, 0, host.c_str(), -1, &s->whost[0], wl);
        s->whost.resize(wl - 1);

        s->hSession = WinHttpOpen(L"RemoteControl/5.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
            WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
        if (!s->hSession) { delete s; continue; }

        WinHttpSetTimeouts(s->hSession, 15000, 15000, 15000, 15000);

        std::string resp;
        if (!httpPost(s->hSession, s->whost, s->port, s->tls, "/register",
                       regPayload.data(), regPayload.size(), resp)) {
            if (g_lastError.empty()) setError("Register failed (attempt %d)", attempt + 1);
            delete s;
            if (attempt < 2) Sleep(500);
            continue;
        }
        if (resp.empty()) {
            setError("Empty response from /register");
            delete s;
            if (attempt < 2) Sleep(500);
            continue;
        }

        s->sessionId = resp;
        setError("OK");
        return s;
    }
    if (g_lastError.empty() || g_lastError == "OK") setError("All retries failed");
    return INVALID;
}

bool sendAll(SocketT sock, const void* data, size_t len) {
    if (!sock) return false;
    char path[256]; snprintf(path, sizeof(path), "/send?id=%s", sock->sessionId.c_str());
    std::string resp;
    return httpPost(sock->hSession, sock->whost, sock->port, sock->tls, path, data, len, resp);
}

// ── Async send queue ────────────────────────────────────────────────
// sendAll is synchronous and blocks the caller for up to 90s on a slow
// relay. The server's ImGui UI calls sendCommand on every button press
// and dozens of times per second during draw-drag — that freezes the
// frame. AsyncSend runs a background thread that drains a queue and
// fires HTTP POSTs without blocking the UI thread.
struct AsyncSendQueue {
    std::mutex mtx;
    std::condition_variable cv;
    std::queue<std::pair<HttpPollSession*, std::vector<uint8_t>>> q;
    std::thread worker;
    std::atomic<bool> running{false};
    HttpPollSession* sock = nullptr;

    void start(HttpPollSession* s) {
        sock = s;
        if (running) return;
        running = true;
        worker = std::thread([this]() {
            while (running) {
                std::pair<HttpPollSession*, std::vector<uint8_t>> item;
                {
                    std::unique_lock<std::mutex> lk(mtx);
                    cv.wait(lk, [this]{ return !q.empty() || !running; });
                    if (!running && q.empty()) break;
                    item = std::move(q.front());
                    q.pop();
                }
                char path[256];
                snprintf(path, sizeof(path), "/send?id=%s", item.first->sessionId.c_str());
                std::string resp;
                httpPost(item.first->hSession, item.first->whost,
                         item.first->port, item.first->tls,
                         path, item.second.data(), item.second.size(), resp);
            }
        });
    }

    void stop() {
        running = false;
        cv.notify_all();
        if (worker.joinable()) worker.join();
    }

    void enqueue(HttpPollSession* s, const void* data, size_t len) {
        {
            std::lock_guard<std::mutex> lk(mtx);
            q.push({s, std::vector<uint8_t>((const uint8_t*)data, (const uint8_t*)data + len)});
        }
        cv.notify_one();
    }
};

static AsyncSendQueue g_asyncSend;

void startAsyncSend(SocketT sock) {
    if (sock) g_asyncSend.start(sock);
}

void stopAsyncSend() {
    g_asyncSend.stop();
}

bool sendAllAsync(SocketT sock, const void* data, size_t len) {
    if (!sock) return false;
    g_asyncSend.enqueue(sock, data, len);
    return true;
}

bool sendMessage(SocketT sock, const std::vector<uint8_t>& msg) {
    return sendAll(sock, msg.data(), msg.size());
}

void close(SocketT sock) {
    if (!sock) return;
    char path[256]; snprintf(path, sizeof(path), "/disconnect?id=%s", sock->sessionId.c_str());
    std::string resp;
    httpPost(sock->hSession, sock->whost, sock->port, sock->tls, path, "", 0, resp);
    delete sock;
}

void Receiver::start(SocketT sock, OnMessage cb, bool* aliveFlag) {
    if (thread_.joinable()) thread_.join();
    sock_ = sock; cb_ = std::move(cb); running_ = true;
    aliveFlag_ = aliveFlag;
    // Counter MUST be heap-allocated (or a member): capturing a stack-local
    // int by reference causes a use-after-free as soon as start() returns.
    auto* consecutiveFails = new int(0);
    thread_ = std::thread([this, consecutiveFails]() mutable {
        while (running_) {
            char path[256];
            snprintf(path, sizeof(path), "/poll?id=%s", sock_->sessionId.c_str());

            std::vector<uint8_t> resp;
            if (!httpGetBin(sock_->hSession, sock_->whost, sock_->port, sock_->tls, path, resp)) {
                if (!running_) break;
                Sleep(200);
                if (++*consecutiveFails > 10) {
                    // Session likely dead on relay side (kicked, expired, or simply
                    // unreachable). Bail and let the outer loop reconnect fresh.
                    running_ = false;
                    break;
                }
                continue;
            }
            *consecutiveFails = 0;
            if (!running_) break;
            if (resp.empty()) continue;  // 204 No Content, no messages

            // Parse messages
            size_t off = 0;
            while (off + proto::HEADER_SIZE <= resp.size()) {
                uint32_t psz; memcpy(&psz, resp.data() + off, 4);
                uint8_t mt = resp[off + 4], ct = resp[off + 5];
                if (off + proto::HEADER_SIZE + psz > resp.size()) break;

                std::vector<uint8_t> pl(resp.data() + off + proto::HEADER_SIZE,
                                        resp.data() + off + proto::HEADER_SIZE + psz);
                if (cb_) {
                    cb_(mt, ct, std::move(pl));
                    if (aliveFlag_ && !*aliveFlag_) {
                        running_ = false;
                        break;
                    }
                }
                off += proto::HEADER_SIZE + psz;
            }
        }
        // No synthetic cb_(0,0,{}) — the consumer manages its own life flag.
        delete consecutiveFails;
    });
}

void Receiver::stop() {
    running_ = false;
    if (thread_.joinable()) thread_.join();
    // Do NOT call cb_(0,0,{}) automatically — let the caller decide.
    cb_ = nullptr;
    sock_ = nullptr;
    aliveFlag_ = nullptr;
}

} // namespace ws

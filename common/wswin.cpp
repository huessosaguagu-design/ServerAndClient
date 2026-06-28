#include "wswin.h"
#include "protocol.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winhttp.h>
#pragma comment(lib, "winhttp.lib")

#include <cstring>
#include <cstdio>
#include <cstdarg>

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

    WinHttpSetTimeouts(hReq, 90000, 90000, 90000, 90000);
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

    DWORD br = 0; char buf[4096]; resp.clear();
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

    WinHttpSetTimeouts(hReq, 90000, 90000, 90000, 40000);

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

    DWORD br = 0; uint8_t buf[65536]; resp.clear();
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

        WinHttpSetTimeouts(s->hSession, 90000, 90000, 90000, 90000);

        std::string resp;
        if (!httpPost(s->hSession, s->whost, s->port, s->tls, "/register",
                       regPayload.data(), regPayload.size(), resp)) {
            if (g_lastError.empty()) setError("Register failed (attempt %d)", attempt + 1);
            delete s;
            if (attempt < 2) Sleep(2000);
            continue;
        }
        if (resp.empty()) {
            setError("Empty response from /register");
            delete s;
            if (attempt < 2) Sleep(2000);
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

void Receiver::start(SocketT sock, OnMessage cb) {
    if (thread_.joinable()) thread_.join();
    sock_ = sock; cb_ = std::move(cb); running_ = true;
    thread_ = std::thread([this]() {
        // First, do one poll to get the initial messages (register ack, client list)
        while (running_) {
            char path[256];
            snprintf(path, sizeof(path), "/poll?id=%s", sock_->sessionId.c_str());

            std::vector<uint8_t> resp;
            if (!httpGetBin(sock_->hSession, sock_->whost, sock_->port, sock_->tls, path, resp)) {
                if (!running_) break;
                Sleep(1000);
                continue;
            }
            if (!running_) break;
            if (resp.empty()) continue;

            // Parse messages
            size_t off = 0;
            while (off + proto::HEADER_SIZE <= resp.size()) {
                uint32_t psz; memcpy(&psz, resp.data() + off, 4);
                uint8_t mt = resp[off + 4], ct = resp[off + 5];
                if (off + proto::HEADER_SIZE + psz > resp.size()) break;

                std::vector<uint8_t> pl(resp.data() + off + proto::HEADER_SIZE,
                                        resp.data() + off + proto::HEADER_SIZE + psz);
                if (cb_) cb_(mt, ct, std::move(pl));
                off += proto::HEADER_SIZE + psz;
            }
        }
        if (cb_) cb_(0, 0, {});
    });
}

void Receiver::stop() {
    running_ = false;
    if (thread_.joinable()) thread_.join();
}

} // namespace ws

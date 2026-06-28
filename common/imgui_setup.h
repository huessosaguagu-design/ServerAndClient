#pragma once
#include "imgui.h"
#include <d3d11.h>
#include <functional>
#include <string>

// ═══════════════════════════════════════════════════════════════════
//  IMGUI SETUP — DX11 + Win32 boilerplate encapsulated in one call
// ═══════════════════════════════════════════════════════════════════

// Called every frame; draw your ImGui UI inside.
using DrawCallback = std::function<void()>;

// Called once before the main loop starts (after ImGui is initialised).
using InitCallback = std::function<void()>;

// Run the ImGui application. Returns process exit code.
int imguiRun(
    const wchar_t* windowTitle,
    int width,
    int height,
    const InitCallback& onInit,
    const DrawCallback& onDraw
);

// ── Texture helpers (for ViewScreen image display) ─────────────────
// Creates a DX11 texture from RGBA pixel data. Returns a srv pointer
// cast to ImTextureID, or nullptr on failure. Caller must Release().
ImTextureID imguiCreateTextureRGBA(const void* rgbaData, int width, int height);
void        imguiReleaseTexture(ImTextureID tex);

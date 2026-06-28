cmake_minimum_required(VERSION 3.15)
project(ServerAndClient LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(IMGUI_DIR ${CMAKE_CURRENT_SOURCE_DIR}/imgui-1.92.8)

# ── ImGui static library (core + DX11 + Win32 backends) ──────────────
add_library(imgui STATIC
    ${IMGUI_DIR}/imgui.cpp
    ${IMGUI_DIR}/imgui_draw.cpp
    ${IMGUI_DIR}/imgui_tables.cpp
    ${IMGUI_DIR}/imgui_widgets.cpp
    ${IMGUI_DIR}/imgui_demo.cpp
    ${IMGUI_DIR}/backends/imgui_impl_dx11.cpp
    ${IMGUI_DIR}/backends/imgui_impl_win32.cpp
)
target_include_directories(imgui PUBLIC
    ${IMGUI_DIR}
    ${IMGUI_DIR}/backends
)

# ── Common library (protocol + networking) ──────────────────────────
add_library(common STATIC
    common/network.cpp
)
target_include_directories(common PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/common)

if(WIN32)
    target_link_libraries(common PUBLIC ws2_32)
    target_compile_definitions(common PUBLIC _WIN32_WINNT=0x0A00) # Windows 10+
endif()

# ── Server (ImGui GUI — Windows only) ───────────────────────────────
if(WIN32)
    add_executable(server WIN32
        server/main.cpp
        common/imgui_setup.cpp
    )
    target_link_libraries(server PRIVATE imgui common)
    target_link_libraries(server PRIVATE d3d11 dxgi d3dcompiler dwmapi)
endif()

# ── Client (ImGui GUI — Windows only) ───────────────────────────────
if(WIN32)
    add_executable(client WIN32
        client/main.cpp
        common/imgui_setup.cpp
    )
    target_link_libraries(client PRIVATE imgui common)
    target_link_libraries(client PRIVATE d3d11 dxgi d3dcompiler dwmapi gdiplus)
endif()

# ── Relay (headless — works on Windows and Linux for Docker/OnRender) ─
add_executable(relay
    relay/main.cpp
)
target_link_libraries(relay PRIVATE common)
if(WIN32)
    target_link_libraries(relay PRIVATE ws2_32)
endif()

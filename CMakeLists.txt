cmake_minimum_required(VERSION 3.20)
project(meow-desk-core LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# 不依赖 core-ui：它的主窗口 swap chain 不带 alpha，窗口无法半透明
# （实测 SetLayeredWindowAttributes / DwmEnableBlurBehindWindow /
#   SetWindowCompositionAttribute 全部无效），且它的 D3D11+D2D 管线
#   固定占用约 49MB 私有内存。
# 桌面层、盒子、拖拽层全部改用纯 Win32 UpdateLayeredWindow + GDI 自绘。

add_executable(meow-desk WIN32
    src/main.cpp
    src/desktop_icons.cpp
    src/shell_icons.cpp
    src/icon_render.cpp
    src/desktop_surface.cpp
    src/drag_layer.cpp
    src/fence_window.cpp
    src/backdrop.cpp
    src/config.cpp
    src/zorder.cpp
    src/tray.cpp
    src/settings_window.cpp
    src/search_window.cpp
)

target_include_directories(meow-desk PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
)

# /MD：与 GDI+ / UCRT 导入符号对齐
set_property(TARGET meow-desk PROPERTY
    MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")

target_link_libraries(meow-desk PRIVATE
    gdiplus user32 gdi32 shell32 ole32 shlwapi dwmapi comctl32 msimg32 uxtheme imm32
)

target_compile_definitions(meow-desk PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)

if(MSVC)
    target_compile_options(meow-desk PRIVATE /W3 /utf-8)
endif()

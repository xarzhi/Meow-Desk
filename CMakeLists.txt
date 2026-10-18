cmake_minimum_required(VERSION 3.20)
# 需要 C：third_party/sqlite 是 .c 文件（只声明 CXX 的话 CMake 会直接不收它）
project(meow-desk-core LANGUAGES C CXX)

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
    src/storage.cpp
    src/zorder.cpp
    src/tray.cpp
    src/settings_window.cpp
    src/search_window.cpp
    src/pinyin.cpp
    src/layout_store.cpp
    # 本地存储用 SQLite 3 的 amalgamation（单文件，无外部依赖）。
    # 比 *.ini 整份重写可靠：事务保证原子性，加字段走 user_version 迁移。
    # 内存代价实测为零 —— 代码页是 file-backed，不占 private。
    third_party/sqlite/sqlite3.c
)

target_include_directories(meow-desk PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
    "${CMAKE_CURRENT_SOURCE_DIR}/third_party/sqlite"
)

# sqlite3.c 是一份 9MB 的第三方 amalgamation，别让 /W3 把时间浪费在它身上
if(MSVC)
    set_source_files_properties(third_party/sqlite/sqlite3.c PROPERTIES
        COMPILE_OPTIONS "/w")
endif()

# /MD：与 GDI+ / UCRT 导入符号对齐
set_property(TARGET meow-desk PROPERTY
    MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")

target_link_libraries(meow-desk PRIVATE
    gdiplus user32 gdi32 shell32 ole32 shlwapi dwmapi comctl32 msimg32 uxtheme imm32
)

target_compile_definitions(meow-desk PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)

# SQLite：单线程使用（只有 UI 线程碰它），砍掉用不到的特性；
# DQS=0 关掉「双引号当字符串」的兼容行为；MEMSTATUS=0 省掉每次 malloc 的统计。
target_compile_definitions(meow-desk PRIVATE
    SQLITE_THREADSAFE=0
    SQLITE_OMIT_LOAD_EXTENSION
    SQLITE_DQS=0
    SQLITE_DEFAULT_MEMSTATUS=0
    SQLITE_OMIT_DEPRECATED
    SQLITE_OMIT_SHARED_CACHE
)

if(MSVC)
    target_compile_options(meow-desk PRIVATE /W3 /utf-8)
endif()

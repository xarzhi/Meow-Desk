cmake_minimum_required(VERSION 3.20)
# 需要 C：third_party/sqlite 是 .c 文件（只声明 CXX 的话 CMake 会直接不收它）
# 需要 RC：res/resources.rc 把 exe 图标、关于页 logo、图标字体编进 exe
project(meow-desk-core LANGUAGES C CXX RC)

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
    src/popup_menu.cpp
    src/confirm_dialog.cpp
    src/pinyin.cpp
    src/layout_store.cpp
    src/file_drop.cpp
    src/rename_box.cpp
    src/shell_drag.cpp
    src/app_icon.cpp
    src/icon_font.cpp
    # exe 图标 / 关于页 logo / 设置页图标字体（见 res/app_res.h）
    res/resources.rc
    # 本地存储用 SQLite 3 的 amalgamation（单文件，无外部依赖）。
    # 比 *.ini 整份重写可靠：事务保证原子性，加字段走 user_version 迁移。
    # 内存代价实测为零 —— 代码页是 file-backed，不占 private。
    third_party/sqlite/sqlite3.c
)

target_include_directories(meow-desk PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
    "${CMAKE_CURRENT_SOURCE_DIR}/res"
    "${CMAKE_CURRENT_SOURCE_DIR}/third_party/sqlite"
)

# sqlite3.c 是一份 9MB 的第三方 amalgamation，别让 /W3 把时间浪费在它身上
if(MSVC)
    set_source_files_properties(third_party/sqlite/sqlite3.c PROPERTIES
        COMPILE_OPTIONS "/w")
endif()

# rc.exe 引用的那些图片/字体**不会**被自动跟踪：cmcldeps 生成的 .d 只记 rc 里
# #include 的头文件，不记 ICON / RCDATA 指向的文件。结果是「换了 ico 或 png，
# ninja 认为资源没变，exe 里还是旧图标」—— 而且完全没有任何报错，只有肉眼看出来。
# 这里手动把它们声明成依赖。
set_source_files_properties(res/resources.rc PROPERTIES OBJECT_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/res/meow-desk.ico;${CMAKE_CURRENT_SOURCE_DIR}/res/logo-128.png;${CMAKE_CURRENT_SOURCE_DIR}/src/iconfont/iconfont.ttf"
)
set_source_files_properties(res/setup_res.rc PROPERTIES OBJECT_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/res/meow-desk.ico"
)

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

# ---------------------------------------------------------------------------
# 安装程序：把一个 meow-desk.exe 复制到用户选定位置，
# 并决定数据放 %APPDATA% 还是 exe 同目录（便携模式）。
# 它从自己所在目录读 meow-desk.exe，所以两者要放在一起分发（见 package.sh）。
# ---------------------------------------------------------------------------
add_executable(meow-setup WIN32 setup/setup.cpp res/setup_res.rc)

# 安装程序也要能找到 res/app_res.h（setup_res.rc 里 include 它）
target_include_directories(meow-setup PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/res")

set_property(TARGET meow-setup PROPERTY
    MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")
target_link_libraries(meow-setup PRIVATE user32 gdi32 shell32 ole32 shlwapi comctl32 dwmapi)
target_compile_definitions(meow-setup PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)

if(MSVC)
    target_compile_options(meow-setup PRIVATE /W3 /utf-8)
endif()

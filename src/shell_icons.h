// Shell 图标加载 + 系统桌面图标隐藏/恢复
#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <string>
#include <vector>
#include <cstdint>

// 一张加载好的图标位图（BGRA，premultiplied，32bpp，自上而下）
struct IconBitmap {
    int width = 0;
    int height = 0;
    std::vector<uint8_t> pixels;  // width*height*4

    bool valid() const { return width > 0 && height > 0 && !pixels.empty(); }
};

// 从 Shell 加载指定文件的图标（IShellItemImageFactory，带 alpha）
// path 可以是文件路径或 ::{CLSID} 系统图标；size 为期望的像素边长（如 48）
// linkOverlay = true 时，若 path 是快捷方式则叠加系统快捷方式箭头
bool LoadShellIcon(const std::wstring& path, int size, IconBitmap& out,
                   bool linkOverlay = false);

// 是否是快捷方式（.lnk / .url）
bool IsShortcutPath(const std::wstring& path);

// 资源管理器桌面图标标签真正用的字体（向 SysListView32 要 WM_GETFONT），
// 拿不到时退回 SPI_GETICONTITLELOGFONT / 默认 Segoe UI 9pt。
// 返回的 HFONT 由进程持有，不要删除。quality 改成灰度抗锯齿。
HFONT DesktopIconFont();

// 隐藏/恢复资源管理器的桌面图标视图（只动图标控件本身，不改系统"显示桌面图标"设置）
bool SetSystemDesktopIconsVisible(bool visible);

// 当前系统桌面图标是否可见
bool IsSystemDesktopIconsVisible();

// 桌面宿主窗口：承载 SHELLDLL_DefView 的那个顶层窗口（通常是 Progman，
// 装了动态壁纸时可能是某个 WorkerW）
HWND FindDesktopHostWindow();

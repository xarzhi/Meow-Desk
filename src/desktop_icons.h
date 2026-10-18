// meow-desk — 轻量桌面分区工具（重写 my-desktop）
// 目标：内存 <10MB，运行快，支持磨砂盒子、收起/展开动画、盒内 input
// 技术栈：C++17 + core-ui（Direct2D 硬件加速 UI 框架）+ Win32 Shell 互操作
#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <windowsx.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <commctrl.h>
#include <dwmapi.h>

#include <string>
#include <vector>
#include <functional>
#include <memory>

// 桌面图标条目
struct DesktopIcon {
    std::wstring path;       // 完整解析名（文件路径或 ::{CLSID}）
    std::wstring name;       // 显示名
    POINT        pos;        // 屏幕物理像素坐标（格子左上角）
    bool         isFolder = false;
    bool         isFileSystem = false;
    void*        pidl = nullptr;  // 资源管理器给的 PIDL，用 FreeDesktopPidl 释放
};

// 枚举桌面图标：通过 IShellFolder 读取系统桌面视图的图标及位置
std::vector<DesktopIcon> EnumerateDesktopIcons();

// 把一批图标的新位置写回资源管理器（IFolderView2::SelectAndPositionItems）。
// 这样真实桌面和我们的桌面层保持一致，重启后位置还在。
bool SetDesktopIconPositions(size_t count, void* const* pidls, const POINT* points);

// 释放 EnumerateDesktopIcons 交出的 PIDL
void FreeDesktopPidl(void* pidl);

// 最近一次枚举的诊断信息（哪一步、HRESULT）
const char* LastEnumDiag();

// 最近一次枚举得到的图标间距（像素）。失败时为 75x98 的 Windows 默认值。
POINT LastEnumSpacing();

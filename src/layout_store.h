// 图标布局持久化
//
// 为什么需要：`owner`（图标属于哪个盒子）以前只存在内存里，程序一退出就全丢，
// 盒子里放好的图标重启后全回到桌面。这里把「归属 + 盒内顺序 + 桌面坐标」
// 写到 %APPDATA%\meow-desk\layout.ini，启动时按 path 还原。
//
// 用 path（解析名）当唯一键：显示名可能重名，path 唯一且稳定。
//
// 文件格式（UTF-8, LF, TAB 分隔）：
//   # meow-desk layout v1
//   owner <TAB> order <TAB> x <TAB> y <TAB> path
// path 放最后一个字段，这样路径里出现空格/中文都没问题（TAB 基本不会出现在路径里）。
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

struct LayoutEntry {
    std::wstring path;
    int owner = -1;   // -1 = 桌面；>=0 = 盒子下标
    int order = 0;    // 盒内顺序（owner >= 0 时有效）
    POINT pos{0, 0};  // 桌面坐标（owner < 0 时有效）
};

// %APPDATA%\meow-desk\layout.ini
std::wstring LayoutFilePath();

bool LoadLayout(std::vector<LayoutEntry>& out);
bool SaveLayout(const std::vector<LayoutEntry>& in);

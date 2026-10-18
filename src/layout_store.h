// 图标布局持久化
//
// 为什么需要：`owner`（图标属于哪个盒子）以前只存在内存里，程序一退出就全丢，
// 盒子里放好的图标重启后全回到桌面。这里把「归属 + 盒内顺序 + 桌面坐标」
// 落到 %APPDATA%\meow-desk\meow-desk.db 的 icon_layout 表，启动时按 path 还原。
//
// 用 path（解析名）当主键：显示名可能重名，path 唯一且稳定。
//
// 表结构见 storage.cpp 的 Upgrade()：
//   icon_layout(path TEXT PK, owner INT, ord INT, x INT, y INT)
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

// 读回全部布局记录。返回 false 表示没有记录（全新用户）。
// 首次调用会顺带把老的 layout.ini 迁移进数据库并改名 .bak。
bool LoadLayout(std::vector<LayoutEntry>& out);

// 整份覆盖写入（一个事务内 DELETE + 批量 INSERT）
bool SaveLayout(const std::vector<LayoutEntry>& in);

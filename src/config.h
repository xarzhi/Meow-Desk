// 配置：%APPDATA%\meow-desk\config.ini
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

// 一个盒子的外观与位置
struct FenceConfig {
    int x = 0;
    int y = 0;
    int w = 340;
    int h = 470;
    int color = RGB(250, 251, 254);  // 底色（RGB）
    int alpha = 236;                 // 0-255
    int radius = 12;                 // 圆角半径
    int acrylic = 0;                 // 1 = Win11 亚克力（模糊壁纸）
    std::wstring title;
};

struct AppConfig {
    int textShadow = 1;         // 桌面图标文字阴影
    int shortcutArrow = 1;      // 快捷方式箭头
    int showLabels = 1;         // 显示图标标题（桌面 + 盒子）
    int headerHeight = 44;      // 盒子头部高度
    int autoExpandOnHover = 0;  // 鼠标移到收起盒子的头部时自动展开，移出整个盒子再收起
    std::vector<FenceConfig> fences;

    void Load();
    void Save() const;

    static std::wstring Dir();
    static std::wstring File();
};

extern AppConfig g_config;

// 启动时把默认值补齐（首次运行）
void ConfigEnsureDefaults();

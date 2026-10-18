// 配置：%APPDATA%\meow-desk\meow-desk.db（SQLite 的 kv 表）
// 老的 config.ini 只在「首次迁移」和「数据库打不开时的回退」里读，不再写。
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
    // 1 = 锁定：右上角的锁按钮按下后，盒子既不能拖动也不能改尺寸
    int locked = 0;
    std::wstring title;

    // 「查看」：0=小 1=中 2=大 3=超大 4=列表 5=详情
    // 每个盒子各自记，右键菜单改的是「当前这个盒子」
    int viewMode = 1;
    // 「排序方式」：0=名称 1=类型 2=大小 3=修改日期 4=自定义
    // 4 表示不排序，保持用户自己拖出来的顺序
    int sortKey = 4;
    int sortAsc = 1;                 // 1 = 正序，0 = 倒序
};

// 整理规则：把匹配的桌面文件归入同名盒子（和 my-desktop 的「整理规则」一致）
//
// 匹配顺序 = 列表顺序，第一条命中的生效；matchFolders 的规则吃下所有文件夹，
// isFallback 的兜底规则吃下谁都没匹配到的项目（永远放在最后，由默认规则保证）。
struct OrganizeRule {
    std::wstring name;
    // 扩展名，分号 / 逗号 / 空格分隔，例如 ".jpg; .png"（不带点也认）
    std::wstring exts;
    int matchFolders = 0;
    int isFallback = 0;
};

// 默认规则（和 my-desktop 的 OrganizeRule.CreateDefaults 一一对应）
std::vector<OrganizeRule> OrganizeRulesDefault();

// 这条规则是否吃下 path；ext 由调用方给（已小写、带点），isDir 表示是文件夹
bool OrganizeRuleMatches(const OrganizeRule& r, const std::wstring& ext, bool isDir);

struct AppConfig {
    int textShadow = 1;         // 桌面图标文字阴影
    int shortcutArrow = 1;      // 快捷方式箭头
    int showLabels = 1;         // 显示图标标题（桌面 + 盒子）
    int headerHeight = 44;      // 盒子头部高度
    int autoExpandOnHover = 0;  // 鼠标移到收起盒子的头部时自动展开，移出整个盒子再收起
    int sortAnim = 1;           // 改排序方式时，盒内图标播位移过渡动画
    // 显示盒子标题：关掉后标题文字和右上角箭头都不画，头部只留 2px 透明条
    // （仍然能收到鼠标，悬停自动展开照样有效）
    int showFenceTitle = 1;
    // 桌面上新出现的文件几秒后自动按整理规则归类（监视桌面文件夹）
    int autoOrganize = 0;

    std::vector<FenceConfig> fences;
    std::vector<OrganizeRule> rules;

    void Load();
    void Save() const;

    static std::wstring Dir();
    static std::wstring File();
};

extern AppConfig g_config;

// 启动时把默认值补齐（首次运行）
void ConfigEnsureDefaults();

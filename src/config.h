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

// 待办盒子里的一条待办
struct TodoItem {
    std::wstring text;
    int done = 0;   // 1 = 已勾掉（画一条删除线 + 变灰）
    // 创建时间：Unix 秒（UTC），显示时按本地时区换算。
    // 0 = 不知道（老库迁移时统一回填过一次，正常不会出现）。
    long long created = 0;
};

// 一个盒子的外观与位置
struct FenceConfig {
    // 盒子类型：0 = 收纳盒（装桌面图标，一直以来的行为）
    //           1 = 待办盒（一个 To-do 清单，不装图标）
    // 待办盒同样有头部的标题 / 锁 / 收起箭头，拖动改尺寸也完全一样 ——
    // 差别只在内容区：收纳盒是图标网格，待办盒是一列可勾选的条目。
    int kind = 0;
    std::vector<TodoItem> todos;   // 只有 kind == 1 才有内容

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
    // 展开方式（头部右侧那个箭头点击切换）：
    //   1 = 展开模式：始终展开，鼠标移出去也不收
    //   0 = 伸缩模式：鼠标移到头部展开，移出整个盒子自动收起
    // 收起后头部只剩 4px（见 FenceWindow::kCollapsedHeadH），头部的标签/标题/
    // 锁/箭头全都不画 —— 整块缩成一条「把手」。
    // -1 = 老数据里没有这个字段，由 ConfigEnsureDefaults 按全局默认补齐
    int expandMode = -1;

    // 收起方向（右键菜单「收缩方向」）：0 = 上，1 = 下，2 = 左，3 = 右。
    // 收起后整个盒子只剩 4px 那一条「把手」，贴在**指定那条边**上：
    //   上 → 留在上边（盒子往上收）   下 → 留在下边（盒子往下收）
    //   左 → 留在左边（盒子往左收）   右 → 留在右边（盒子往右收）
    // 鼠标移到这条 4px 上就展开，所以方向决定「把手停在哪一面」。
    // 老库由 v8 迁移补这一列，默认 0 = 上（和以前的位置完全一致）。
    int collapseDir = 0;

    // 盒子内的分组（头部那一排标签）。空 = 没有分组，标题栏直接显示 title。
    // 一旦有分组，groups[0] 就是盒子自己的标题，后面的才是「创建分组」加出来的。
    std::vector<std::wstring> groups;
    int activeGroup = 0;   // 当前显示第几个分组

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
    // 新建盒子的默认展开方式：1 = 伸缩模式，0 = 展开模式（始终展开）。
    // 历史上这一项是「鼠标移到头部自动展开」的全局开关；现在这件事由每个盒子
    // 头部右侧的箭头自己决定，它退化成「新建盒子时用哪种方式」的默认值。
    // 老配置里的 autoExpandOnHover 键仍然认（见 config.cpp 的读取分支）。
    int defaultTelescope = 0;
    int sortAnim = 1;           // 改排序方式时，盒内图标播位移过渡动画
    // 拖放 / 插入的 flip 位移动画：拖着图标插到别的图标之间时，被挤开的那些
    // 用翻转位移走过去。和 sortAnim 一起受交互页那个「排序 / 拖放动画」开关控制，
    // 分两个字段是为了以后要单独关掉其中一个时不用再动配置格式。
    int flipAnim = 1;
    // 自动吸附：移动盒子 / 改尺寸时，靠近屏幕边或别的盒子 10px 以内就贴上去。
    // 关掉之后就纯自由摆放，绝不改用户拖到的坐标。
    int autoSnap = 1;
    // 显示盒子标题：关掉后标题文字和右上角箭头都不画，头部只留 2px 透明条
    // （仍然能收到鼠标，悬停自动展开照样有效）
    int showFenceTitle = 1;
    // 桌面上新出现的文件几秒后自动按整理规则归类（监视桌面文件夹）
    int autoOrganize = 0;
    // 分组标签的切换方式：1 = 鼠标移入标签就切，0 = 点击才切
    int switchTabOnHover = 1;
    // 以高优先级运行：桌面层要跟手，NORMAL 优先级下偶尔会被别的程序抢 CPU 拖慢重绘
    int highPriority = 0;

    // 唤出搜索面板的全局快捷键，形如 "ALT+F" / "CTRL+ALT+SPACE"。
    // 存字符串而不是 (mods,vk) 两个整数：db 里可读、以后加项也不用改表结构。
    // 解析失败时调用方会退回 ALT+F（见 ParseHotkey）。
    std::wstring searchHotkey = L"ALT+F";

    std::vector<FenceConfig> fences;
    std::vector<OrganizeRule> rules;

    void Load();
    void Save() const;

    static std::wstring Dir();
    static std::wstring File();
};

// 全局快捷键字符串 ⇄ win32 的 (modifiers, vk)。
//
// 为什么用字符串：db 的 app 表是纯文本 kv，存 "ALT+F" 人能看懂、改起来也方便，
// 以后要加更多快捷键也不用改表结构。
//
// 支持的键名：A-Z / 0-9 / F1-F24 / SPACE TAB ENTER ESC LEFT RIGHT UP DOWN
//             HOME END PGUP PGDN INSERT DELETE 和几个 OEM 符号；
// 表里没有的一律回退成 `VK_0x<hex>`，所以 Parse 和 Format 永远是互逆的。
bool ParseHotkey(const std::wstring& s, UINT& mods, UINT& vk);
std::wstring FormatHotkey(UINT mods, UINT vk);

extern AppConfig g_config;

// 启动时把默认值补齐（首次运行）
void ConfigEnsureDefaults();

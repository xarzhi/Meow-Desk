// 设置窗口（Fluent 风格，左侧导航五页）
//
// 不依赖 core-ui：自绘一个轻量面板就够了，且能保证和盒子/桌面层用同一套
// 视觉语言（圆角、accent、灰度层次）。
//
// 「整理规则」页需要真正的输入框（中文规则名要能打），所以那几行用的是
// 真正的 EDIT 子控件，贴在自绘的卡片上（和搜索面板里放 EDIT 是同一个理由）。
#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <functional>
#include <vector>

namespace Gdiplus {
class Graphics;
}
struct SettingsTheme;

class SettingsWindow {
public:
    enum class Page { General = 0, Appearance, Interaction, Rules, About, Count };

    bool Create(HWND owner);
    void Destroy();
    void Show();
    void Hide();
    bool visible() const { return hwnd_ && IsWindowVisible(hwnd_); }
    HWND hwnd() const { return hwnd_; }

    // 任何设置变化（外部据此让盒子/桌面层立刻生效）
    std::function<void()> onChanged;
    // 常规页的两个动作
    std::function<void()> onReloadIcons;
    std::function<bool()> getSystemIconsShown;
    std::function<void(bool)> setSystemIconsShown;
    // 整理规则页：「立即整理桌面…」
    std::function<void()> onOrganize;

private:
    static LRESULT CALLBACK WndProcStatic(HWND, UINT, WPARAM, LPARAM);
    LRESULT WndProc(HWND, UINT, WPARAM, LPARAM);

    bool EnsureBackbuffer(int w, int h);
    void Layout();
    void Paint();
    void PaintNav(Gdiplus::Graphics& g, const SettingsTheme& t);
    void PaintPage(Gdiplus::Graphics& g, const SettingsTheme& t);
    void PaintRulesPage(Gdiplus::Graphics& g, const SettingsTheme& t);
    void NotifyChanged();
    void Toggle(int id);

    // ---- 整理规则的输入行（EDIT 子控件）----
    struct RuleRow {
        int ruleIndex = -1;
        RECT name{};
        RECT exts{};
        RECT del{};
        HWND nameEdit = nullptr;
        HWND extEdit = nullptr;
    };
    void BuildRuleControls();      // 建/重建当前可见的行
    void DestroyRuleControls();
    void SyncRulesFromControls();  // EDIT → g_config.rules
    int RulesVisibleRows() const;
    void ScrollRules(int delta);

    int HitTest(POINT pt) const;   // 返回控件 id，-1 无

    HWND hwnd_ = nullptr;
    HWND owner_ = nullptr;
    bool dark_ = false;
    Page page_ = Page::General;

    HDC memDC_ = nullptr;
    HBITMAP dib_ = nullptr;
    void* bits_ = nullptr;
    int bufW_ = 0;
    int bufH_ = 0;

    // ---- 导航 ----
    RECT rcNav_[5]{};

    // ---- 常规 ----
    RECT rcShadow_{};
    RECT rcArrow_{};
    RECT rcReload_{};
    RECT rcSysIcons_{};

    // ---- 外观 ----
    RECT swatches_[12]{};
    RECT rcRadius_{};
    RECT rcAlpha_{};
    RECT rcHeaderH_{};
    RECT rcAcrylic_{};
    RECT rcShowLabels_{};
    RECT rcShowFenceTitle_{};
    // ---- 交互 ----
    RECT rcHoverExpand_{};
    RECT rcSortAnim_{};

    // ---- 整理规则 ----
    RECT rcAutoOrganize_{};
    RECT rcRulesList_{};      // 列表可视区域
    RECT rcAddRule_{};
    RECT rcResetRules_{};
    RECT rcOrganizeNow_{};
    std::vector<RuleRow> ruleRows_;
    int rulesScroll_ = 0;
    HFONT ruleFont_ = nullptr;
    HBRUSH ruleBrush_ = nullptr;

    // ---- 关于 ----
    RECT rcOpenFolder_{};

    int hot_ = -1;
    int sliderDrag_ = -1;
};

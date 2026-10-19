#include "settings_window.h"

#include <windowsx.h>
#include <dwmapi.h>
#include <objidl.h>
#include <propidl.h>
#include <shellapi.h>
#include <gdiplus.h>

#include <algorithm>
#include <cmath>
#include <cstdio>

#include "config.h"
#include "app_icon.h"
#include "icon_font.h"
#include "icon_render.h"
#include "screen_util.h"

#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "gdiplus.lib")
#pragma comment(lib, "shell32.lib")

using namespace Gdiplus;

namespace {

const wchar_t* kClass = L"MeowDeskSettings";

const int kClientW = 860;
const int kClientH = 620;
const int kNavW = 208;
const int kContentX = 232;
const int kCardR = 28;                    // 卡片右边距 → 卡片右边界 = kClientW - kCardR
const int kCardW = kClientW - kContentX - kCardR;
const int kRowLeft = kContentX + 20;   // 卡片内文字左边

enum : int {
    ID_NONE = -1,
    ID_SWATCH0 = 0,   // 0..11
    ID_RADIUS = 20,
    ID_ALPHA = 21,
    ID_ACRYLIC = 22,
    ID_SHADOW = 23,
    ID_ARROW = 24,
    ID_HEADER_H = 25,
    ID_SHOW_LABELS = 26,
    ID_HOVER_EXPAND = 27,
    ID_SORT_ANIM = 28,
    ID_TAB_HOVER = 29,
    ID_SHOW_FENCE_TITLE = 30,
    ID_AUTO_ORGANIZE = 31,
    ID_ADD_RULE = 32,
    ID_RESET_RULES = 33,
    ID_ORGANIZE_NOW = 34,
    ID_RULE_DEL = 100,    // 100..（每行一个：100 + 行号）
    ID_RELOAD = 40,
    ID_SYSICONS = 41,
    ID_OPENFOLDER = 42,
    ID_AUTOSTART = 43,
    ID_HIGHPRIORITY = 44,
    ID_NAV0 = 60,     // 60..64 = 导航项
    // 规则行的 EDIT 子控件：5000 + 行号*2（名字）/ +1（扩展名）
    ID_RULE_EDIT = 5000,
};

// Fluent 调色：前 8 个是浅色常用底色，后 4 个是明显更暗的备选
// （暗色卡片上浅色底会「发白」，需要几个压得住的深色）。
const COLORREF kSwatchColors[12] = {
    RGB(250, 251, 254), RGB(232, 240, 254), RGB(224, 247, 250), RGB(232, 245, 233),
    RGB(255, 248, 225), RGB(252, 232, 230), RGB(243, 233, 245), RGB(224, 224, 224),
    RGB(56, 62, 78),    RGB(38, 45, 56),    RGB(44, 56, 48),    RGB(58, 44, 52),
};

const wchar_t* kNavNames[5] = {
    L"\u5e38\u89c4",        // 常规
    L"\u5916\u89c2",        // 外观
    L"\u4ea4\u4e92",        // 交互
    L"\u6574\u7406\u89c4\u5219",  // 整理规则
    L"\u5173\u4e8e",        // 关于
};

// 导航项图标：优先用 iconfont（src/iconfont/iconfont.ttf，编在 exe 资源里）。
// 下面那套几何自绘只在字体加载失败时兜底 —— 它的线条粗细得跟着尺寸手调，
// 加一项就得再写一段画法，图标字体换图标只要换码位。
enum class NavGlyph { Sliders, Palette, Cursor, Rules, Info };
const NavGlyph kNavGlyphs[5] = {NavGlyph::Sliders, NavGlyph::Palette, NavGlyph::Cursor,
                                NavGlyph::Rules, NavGlyph::Info};
const wchar_t kNavIcons[5] = {IconCode::kGeneral, IconCode::kAppearance, IconCode::kInteract,
                              IconCode::kRules, IconCode::kAbout};

bool AppsUseLightTheme() {
    HKEY k = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER,
                      L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                      0, KEY_READ, &k) != ERROR_SUCCESS)
        return true;
    DWORD v = 1, sz = sizeof(v);
    RegQueryValueExW(k, L"AppsUseLightTheme", nullptr, nullptr, (LPBYTE)&v, &sz);
    RegCloseKey(k);
    return v != 0;
}

void AddRound(GraphicsPath& p, const RectF& r, float rad) {
    float d = std::min({rad * 2.0f, r.Width, r.Height});
    if (d <= 1.0f) {
        p.AddRectangle(r);
        return;
    }
    p.AddArc(r.X, r.Y, d, d, 180.0f, 90.0f);
    p.AddArc(r.X + r.Width - d, r.Y, d, d, 270.0f, 90.0f);
    p.AddArc(r.X + r.Width - d, r.Y + r.Height - d, d, d, 0.0f, 90.0f);
    p.AddArc(r.X, r.Y + r.Height - d, d, d, 90.0f, 90.0f);
    p.CloseFigure();
}

Color C(COLORREF c) { return Color(255, GetRValue(c), GetGValue(c), GetBValue(c)); }
Color CA(COLORREF c, int a) {
    return Color((BYTE)a, GetRValue(c), GetGValue(c), GetBValue(c));
}

// 滑块轨道（比命中区域窄一点，右侧留出数值文本）
RECT TrackRect(const RECT& row, int rightInset = 92) {
    RECT r = row;
    const int cy = (row.top + row.bottom) / 2;
    r.top = cy - 2;
    r.bottom = cy + 2;
    r.right -= rightInset;
    return r;
}

}  // namespace

struct SettingsTheme {
    COLORREF bg;
    COLORREF nav;
    COLORREF card;
    COLORREF cardBorder;
    COLORREF text;
    COLORREF sub;
    COLORREF accent;
    COLORREF track;
    COLORREF sel;      // 导航选中 pill
    COLORREF hover;    // 导航悬停
};

static SettingsTheme LightTheme() {
    return {RGB(243, 243, 243), RGB(234, 234, 234), RGB(255, 255, 255), RGB(229, 229, 229),
            RGB(26, 26, 26), RGB(97, 97, 97), RGB(0, 103, 192), RGB(224, 224, 224),
            RGB(255, 255, 255), RGB(0, 0, 0)};
}

static SettingsTheme DarkTheme() {
    return {RGB(32, 32, 32), RGB(38, 38, 38), RGB(43, 43, 43), RGB(58, 58, 58),
            RGB(245, 245, 245), RGB(160, 160, 160), RGB(76, 194, 255), RGB(69, 69, 69),
            RGB(58, 58, 58), RGB(255, 255, 255)};
}

LRESULT CALLBACK SettingsWindow::WndProcStatic(HWND h, UINT m, WPARAM w, LPARAM l) {
    SettingsWindow* self =
        reinterpret_cast<SettingsWindow*>(GetWindowLongPtrW(h, GWLP_USERDATA));
    if (m == WM_NCCREATE) {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(l);
        self = reinterpret_cast<SettingsWindow*>(cs->lpCreateParams);
        SetWindowLongPtrW(h, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    }
    if (self) return self->WndProc(h, m, w, l);
    return DefWindowProcW(h, m, w, l);
}

// ---------------------------------------------------------------------------
// 布局
// ---------------------------------------------------------------------------
void SettingsWindow::Layout() {
    // 导航
    // 顶部的「设置 / meow-desk」那两行标题删掉了，导航项跟着上移，
    // 第一项和右侧页面标题（y=26）对齐，不然上面会空一大块。
    const int kNavTop = 26;
    for (int i = 0; i < 5; ++i) {
        rcNav_[i] = RECT{12, kNavTop + i * 48, kNavW - 12, kNavTop + i * 48 + 42};
    }

    // ---- 常规页 ----
    {
        const int c1 = 100;
        rcShadow_ = RECT{kContentX + kCardW - 60, c1 + 46, kContentX + kCardW - 24, c1 + 68};
        rcArrow_ = RECT{kContentX + kCardW - 60, c1 + 108, kContentX + kCardW - 24, c1 + 130};

        const int c2 = 300;
        rcReload_ = RECT{kContentX + kCardW - 148, c2 + 42, kContentX + kCardW - 24, c2 + 74};
        rcSysIcons_ = RECT{kContentX + kCardW - 60, c2 + 104, kContentX + kCardW - 24, c2 + 126};

        // 第二张卡片下半截是「启动」那两块：分隔线之后两行。
        // 两行行距沿用上面的 62，视觉上和一整张卡片里的其它行对齐。
        rcStartDivider_ = RECT{kRowLeft, c2 + 150, kContentX + kCardW - 24, c2 + 151};
        rcAutoStart_ =
            RECT{kContentX + kCardW - 60, c2 + 166, kContentX + kCardW - 24, c2 + 188};
        rcHighPriority_ =
            RECT{kContentX + kCardW - 60, c2 + 228, kContentX + kCardW - 24, c2 + 250};
    }

    // ---- 外观页 ----
    {
        const int c1 = 100;
        for (int i = 0; i < 12; ++i) {
            swatches_[i] = RECT{kContentX + 112 + i * 38, c1 + 44, kContentX + 112 + i * 38 + 26,
                                c1 + 70};
        }
        const int tl = kContentX + 112;
        const int tr = kContentX + kCardW - 24;
        rcRadius_ = RECT{tl, c1 + 100, tr, c1 + 130};
        rcAlpha_ = RECT{tl, c1 + 152, tr, c1 + 182};
        rcHeaderH_ = RECT{tl, c1 + 204, tr, c1 + 234};
        rcAcrylic_ = RECT{tr - 36, c1 + 250, tr, c1 + 272};

        const int c2 = 436;
        rcShowLabels_ = RECT{tr - 36, c2 + 62, tr, c2 + 84};
        rcShowFenceTitle_ = RECT{tr - 36, c2 + 116, tr, c2 + 138};
    }

    // ---- 交互页 ----
    {
        const int c1 = 100;
        const int tr = kContentX + kCardW - 24;
        rcHoverExpand_ = RECT{tr - 36, c1 + 46, tr, c1 + 68};
        rcSortAnim_ = RECT{tr - 36, c1 + 98, tr, c1 + 120};
        rcTabHover_ = RECT{tr - 36, c1 + 150, tr, c1 + 172};
    }

    // ---- 关于页 ----
    {
        const int c1 = 100;
        rcOpenFolder_ = RECT{kContentX + 20, c1 + 172, kContentX + 180, c1 + 204};
    }

    // ---- 整理规则页 ----
    {
        const int tr = kContentX + kCardW - 24;
        rcAutoOrganize_ = RECT{tr - 36, 146, tr, 168};
        rcRulesList_ = RECT{kContentX, 214, kContentX + kCardW, 498};
        rcAddRule_ = RECT{kContentX, 512, kContentX + 104, 542};
        rcResetRules_ = RECT{kContentX + 112, 512, kContentX + 248, 542};
        rcOrganizeNow_ = RECT{kContentX + 256, 512, kContentX + 396, 542};
    }
}

int SettingsWindow::HitTest(POINT pt) const {
    for (int i = 0; i < 5; ++i) {
        const RECT& r = rcNav_[i];
        if (pt.x >= r.left && pt.x < r.right && pt.y >= r.top && pt.y < r.bottom)
            return ID_NAV0 + i;
    }

    auto in = [&](const RECT& r, int padX, int padY) {
        return pt.x >= r.left - padX && pt.x < r.right + padX && pt.y >= r.top - padY &&
               pt.y < r.bottom + padY;
    };

    switch (page_) {
    case Page::General:
        if (in(rcShadow_, 150, 18)) return ID_SHADOW;
        if (in(rcArrow_, 150, 18)) return ID_ARROW;
        if (in(rcReload_, 0, 4)) return ID_RELOAD;
        if (in(rcSysIcons_, 150, 18)) return ID_SYSICONS;
        if (in(rcAutoStart_, 150, 18)) return ID_AUTOSTART;
        if (in(rcHighPriority_, 150, 18)) return ID_HIGHPRIORITY;
        break;
    case Page::Appearance:
        for (int i = 0; i < 12; ++i) {
            if (in(swatches_[i], 4, 4)) return ID_SWATCH0 + i;
        }
        if (in(rcRadius_, 0, 8)) return ID_RADIUS;
        if (in(rcAlpha_, 0, 8)) return ID_ALPHA;
        if (in(rcHeaderH_, 0, 8)) return ID_HEADER_H;
        if (in(rcAcrylic_, 150, 16)) return ID_ACRYLIC;
        if (in(rcShowLabels_, 150, 16)) return ID_SHOW_LABELS;
        if (in(rcShowFenceTitle_, 150, 16)) return ID_SHOW_FENCE_TITLE;
        break;
    case Page::Interaction:
        if (in(rcHoverExpand_, 150, 18)) return ID_HOVER_EXPAND;
        if (in(rcSortAnim_, 150, 18)) return ID_SORT_ANIM;
        if (in(rcTabHover_, 150, 18)) return ID_TAB_HOVER;
        break;
    case Page::Rules:
        if (in(rcAutoOrganize_, 150, 16)) return ID_AUTO_ORGANIZE;
        if (in(rcAddRule_, 0, 4)) return ID_ADD_RULE;
        if (in(rcResetRules_, 0, 4)) return ID_RESET_RULES;
        if (in(rcOrganizeNow_, 0, 4)) return ID_ORGANIZE_NOW;
        for (size_t i = 0; i < ruleRows_.size(); ++i) {
            if (in(ruleRows_[i].del, 4, 4)) return ID_RULE_DEL + (int)i;
        }
        break;
    case Page::About:
        if (in(rcOpenFolder_, 0, 4)) return ID_OPENFOLDER;
        break;
    default:
        break;
    }
    return ID_NONE;
}

void SettingsWindow::NotifyChanged() {
    if (onChanged) onChanged();
}

bool SettingsWindow::EnsureBackbuffer(int w, int h) {
    if (w <= 0 || h <= 0) return false;
    if (memDC_ && bufW_ == w && bufH_ == h) return true;
    if (memDC_) { DeleteDC(memDC_); memDC_ = nullptr; }
    if (dib_) { DeleteObject(dib_); dib_ = nullptr; }
    bits_ = nullptr;
    if (!CreateDib(w, h, memDC_, dib_, bits_)) {
        bufW_ = bufH_ = 0;
        return false;
    }
    bufW_ = w;
    bufH_ = h;
    return true;
}

// ---------------------------------------------------------------------------
// 绘制：左侧导航
// ---------------------------------------------------------------------------
void SettingsWindow::PaintNav(Graphics& g, const SettingsTheme& t) {
    SolidBrush navBg(C(t.nav));
    g.FillRectangle(&navBg, Rect(0, 0, kNavW, kClientH));

    FontFamily family(L"Microsoft YaHei UI");
    Font fItem(&family, 14.0f, FontStyleRegular, UnitPixel);

    StringFormat sf;
    sf.SetLineAlignment(StringAlignmentCenter);
    sf.SetFormatFlags(StringFormatFlagsNoWrap);

    SolidBrush brText(C(t.text));
    SolidBrush brSub(C(t.sub));

    // 导航上方原本有「设置 / meow-desk」两行标题，已按用户要求删掉：
    // 窗口标题栏已经写着程序名，导航自己不需要再报一遍名字。
    for (int i = 0; i < 5; ++i) {
        const RECT& r = rcNav_[i];
        const bool sel = ((int)page_ == i);
        const bool hov = (hot_ == ID_NAV0 + i);

        if (sel || hov) {
            GraphicsPath p;
            AddRound(p, RectF((float)r.left, (float)r.top, (float)(r.right - r.left),
                              (float)(r.bottom - r.top)),
                     6.0f);
            SolidBrush b(sel ? C(t.sel) : CA(t.hover, 20));
            g.FillPath(&b, &p);
            if (sel) {
                // 左侧 accent 竖条
                GraphicsPath bar;
                AddRound(bar, RectF((float)r.left + 1.0f, (float)r.top + 11.0f, 3.0f, 20.0f),
                         1.5f);
                SolidBrush ab(C(t.accent));
                g.FillPath(&ab, &bar);
            }
        }

        // 图标
        const float cx = (float)r.left + 26.0f;
        const float cy = (float)(r.top + r.bottom) * 0.5f;
        const ARGB iconColor = (sel || hov ? C(t.accent) : C(t.sub)).GetValue();
        if (IconFont::Ready()) {
            IconFont::Draw(g, kNavIcons[i], cx, cy, 16.0f, iconColor);
        } else {
        Pen gp(sel || hov ? C(t.accent) : C(t.sub), 1.5f);
        gp.SetStartCap(LineCapRound);
        gp.SetEndCap(LineCapRound);
        SolidBrush gb(sel || hov ? C(t.accent) : C(t.sub));
        switch (kNavGlyphs[i]) {
        case NavGlyph::Sliders:
            for (int k = 0; k < 3; ++k) {
                float y = cy - 5.0f + k * 5.0f;
                g.DrawLine(&gp, cx - 6.0f, y, cx + 6.0f, y);
            }
            g.FillEllipse(&gb, cx - 4.0f, cy - 6.5f, 3.5f, 3.5f);
            g.FillEllipse(&gb, cx + 1.0f, cy - 1.5f, 3.5f, 3.5f);
            g.FillEllipse(&gb, cx - 2.0f, cy + 3.5f, 3.5f, 3.5f);
            break;
        case NavGlyph::Palette: {
            GraphicsPath p;
            p.AddEllipse(cx - 6.5f, cy - 6.5f, 13.0f, 13.0f);
            g.DrawPath(&gp, &p);
            g.FillEllipse(&gb, cx - 4.0f, cy - 4.0f, 3.0f, 3.0f);
            g.FillEllipse(&gb, cx + 1.0f, cy - 4.5f, 3.0f, 3.0f);
            g.FillEllipse(&gb, cx - 1.0f, cy + 1.0f, 3.0f, 3.0f);
            break;
        }
        case NavGlyph::Cursor: {
            PointF tri[3] = {PointF(cx - 4.0f, cy - 7.0f), PointF(cx - 4.0f, cy + 6.0f),
                             PointF(cx + 6.0f, cy - 1.0f)};
            g.FillPolygon(&gb, tri, 3);
            break;
        }
        case NavGlyph::Rules: {
            // 清单：三行，每行前面一个小点，后面一条横线
            for (int k = 0; k < 3; ++k) {
                const float y = cy - 5.0f + k * 5.0f;
                g.FillEllipse(&gb, cx - 6.5f, y - 1.5f, 3.0f, 3.0f);
                g.DrawLine(&gp, cx - 1.5f, y, cx + 6.0f, y);
            }
            break;
        }
        case NavGlyph::Info: {
            GraphicsPath p;
            p.AddEllipse(cx - 6.5f, cy - 6.5f, 13.0f, 13.0f);
            g.DrawPath(&gp, &p);
            g.FillEllipse(&gb, cx - 1.5f, cy - 4.5f, 3.0f, 3.0f);
            g.FillRectangle(&gb, cx - 1.5f, cy - 0.5f, 3.0f, 5.0f);
            break;
        }
        }
        }

        g.DrawString(kNavNames[i], -1, &fItem,
                     RectF((float)r.left + 46.0f, (float)r.top,
                           (float)(r.right - r.left) - 52.0f, (float)(r.bottom - r.top)),
                     &sf, sel ? &brText : &brSub);
    }
}

// ---------------------------------------------------------------------------
// 绘制：右侧页面
// ---------------------------------------------------------------------------
void SettingsWindow::PaintPage(Graphics& g, const SettingsTheme& t) {
    FontFamily family(L"Microsoft YaHei UI");
    Font fPage(&family, 19.0f, FontStyleBold, UnitPixel);
    Font fSection(&family, 14.5f, FontStyleBold, UnitPixel);
    Font fBody(&family, 13.5f, FontStyleRegular, UnitPixel);
    Font fSub(&family, 12.0f, FontStyleRegular, UnitPixel);
    Font fValue(&family, 12.5f, FontStyleRegular, UnitPixel);

    StringFormat sf;
    sf.SetLineAlignment(StringAlignmentCenter);
    sf.SetFormatFlags(StringFormatFlagsNoWrap);
    StringFormat sfRight;
    sfRight.SetAlignment(StringAlignmentFar);
    sfRight.SetLineAlignment(StringAlignmentCenter);
    StringFormat sfCenter;
    sfCenter.SetAlignment(StringAlignmentCenter);
    sfCenter.SetLineAlignment(StringAlignmentCenter);

    SolidBrush brText(C(t.text));
    SolidBrush brSub(C(t.sub));
    SolidBrush brAccent(C(t.accent));

    auto drawCard = [&](int y, int ch) {
        GraphicsPath p;
        AddRound(p, RectF((float)kContentX, (float)y, (float)kCardW, (float)ch), 10.0f);
        SolidBrush fill(C(t.card));
        g.FillPath(&fill, &p);
        Pen pen(C(t.cardBorder), 1.0f);
        g.DrawPath(&pen, &p);
    };

    auto drawToggle = [&](const RECT& r, bool on) {
        const float tw = (float)(r.right - r.left);
        const float th = (float)(r.bottom - r.top);
        GraphicsPath p;
        AddRound(p, RectF((float)r.left, (float)r.top, tw, th), th * 0.5f);
        SolidBrush track(C(on ? t.accent : t.track));
        g.FillPath(&track, &p);
        if (!on) {
            Pen pen(C(dark_ ? RGB(90, 90, 90) : RGB(200, 200, 200)), 1.0f);
            g.DrawPath(&pen, &p);
        }
        const float d = th - 8.0f;
        const float cx = on ? (float)r.right - 4.0f - d * 0.5f : (float)r.left + 4.0f + d * 0.5f;
        const float cy = (float)r.top + th * 0.5f;
        SolidBrush knob(C(on ? RGB(255, 255, 255) : RGB(120, 120, 120)));
        g.FillEllipse(&knob, cx - d * 0.5f, cy - d * 0.5f, d, d);
    };

    auto drawSlider = [&](const RECT& row, int lo, int hi, int val, const std::wstring& text) {
        RECT tr = TrackRect(row);
        GraphicsPath bgp;
        AddRound(bgp, RectF((float)tr.left, (float)tr.top, (float)(tr.right - tr.left), 4.0f),
                 2.0f);
        SolidBrush track(C(t.track));
        g.FillPath(&track, &bgp);

        float t01 = (hi > lo) ? (float)(val - lo) / (float)(hi - lo) : 0.0f;
        t01 = std::clamp(t01, 0.0f, 1.0f);
        const float fw = (float)(tr.right - tr.left) * t01;
        if (fw > 1.0f) {
            GraphicsPath fgp;
            AddRound(fgp, RectF((float)tr.left, (float)tr.top, fw, 4.0f), 2.0f);
            SolidBrush fill(C(t.accent));
            g.FillPath(&fill, &fgp);
        }
        const float cx = (float)tr.left + fw;
        const float cy = (float)(row.top + row.bottom) * 0.5f;
        SolidBrush knob(C(RGB(255, 255, 255)));
        g.FillEllipse(&knob, cx - 9.0f, cy - 9.0f, 18.0f, 18.0f);
        Pen kp(C(dark_ ? RGB(70, 70, 70) : RGB(180, 180, 180)), 1.0f);
        g.DrawEllipse(&kp, cx - 9.0f, cy - 9.0f, 18.0f, 18.0f);
        SolidBrush dot(C(t.accent));
        g.FillEllipse(&dot, cx - 4.0f, cy - 4.0f, 8.0f, 8.0f);

        g.DrawString(text.c_str(), -1, &fValue,
                     RectF((float)(tr.right + 8), (float)row.top, 84.0f,
                           (float)(row.bottom - row.top)),
                     &sfRight, &brSub);
    };

    const COLORREF base = g_config.fences.empty()
                              ? RGB(250, 251, 254)
                              : (COLORREF)g_config.fences[0].color;
    const int radius = g_config.fences.empty() ? 12 : g_config.fences[0].radius;
    const int alpha = g_config.fences.empty() ? 236 : g_config.fences[0].alpha;
    const bool acrylic = !g_config.fences.empty() && g_config.fences[0].acrylic != 0;

    switch (page_) {
    case Page::General: {
        g.DrawString(L"\u5e38\u89c4", -1, &fPage, RectF((float)kContentX, 26.0f, 300.0f, 30.0f),
                     &sf, &brText);
        g.DrawString(L"\u56fe\u6807\u4e0e\u684c\u9762\u884c\u4e3a", -1, &fSub,
                     RectF((float)kContentX + 2, 56.0f, 400.0f, 18.0f), &sf, &brSub);

        const int c1 = 100;
        drawCard(c1, 172);
        g.DrawString(L"\u56fe\u6807", -1, &fSection,
                     RectF((float)kContentX + 20, (float)c1 + 16.0f, 240.0f, 22.0f), &sf,
                     &brText);

        g.DrawString(L"\u56fe\u6807\u6587\u5b57\u9634\u5f71", -1, &fBody,
                     RectF((float)kRowLeft, (float)(rcShadow_.top - 10), 320.0f, 26.0f),
                     &sf, &brText);
        g.DrawString(L"\u767d\u5b57\u5916\u4e00\u5708\u67d4\u548c\u6697\u8fb9", -1, &fSub,
                     RectF((float)kRowLeft, (float)(rcShadow_.top + 12), 340.0f, 16.0f),
                     &sf, &brSub);
        drawToggle(rcShadow_, g_config.textShadow != 0);

        g.DrawString(L"\u5feb\u6377\u65b9\u5f0f\u7bad\u5934", -1, &fBody,
                     RectF((float)kRowLeft, (float)(rcArrow_.top - 10), 320.0f, 26.0f),
                     &sf, &brText);
        g.DrawString(L"\u56fe\u6807\u5de6\u4e0b\u89d2\u663e\u793a\u5feb\u6377\u65b9\u5f0f\u6807\u8bb0",
                     -1, &fSub,
                     RectF((float)kRowLeft, (float)(rcArrow_.top + 12), 340.0f, 16.0f),
                     &sf, &brSub);
        drawToggle(rcArrow_, g_config.shortcutArrow != 0);

        const int c2 = 300;
        drawCard(c2, 272);
        g.DrawString(L"\u684c\u9762", -1, &fSection,
                     RectF((float)kContentX + 20, (float)c2 + 16.0f, 240.0f, 22.0f), &sf,
                     &brText);

        g.DrawString(L"\u5237\u65b0\u684c\u9762\u56fe\u6807", -1, &fBody,
                     RectF((float)kRowLeft, (float)(rcReload_.top - 4), 320.0f, 26.0f),
                     &sf, &brText);
        g.DrawString(L"\u91cd\u65b0\u679a\u4e3e\u771f\u5b9e\u684c\u9762", -1, &fSub,
                     RectF((float)kRowLeft, (float)(rcReload_.top + 18), 300.0f, 16.0f),
                     &sf, &brSub);
        {
            GraphicsPath p;
            AddRound(p, RectF((float)rcReload_.left, (float)rcReload_.top,
                              (float)(rcReload_.right - rcReload_.left),
                              (float)(rcReload_.bottom - rcReload_.top)),
                     6.0f);
            SolidBrush fill(C(t.cardBorder));
            g.FillPath(&fill, &p);
            Pen pen(C(t.cardBorder), 1.0f);
            g.DrawPath(&pen, &p);
            g.DrawString(L"\u5237\u65b0", -1, &fBody,
                         RectF((float)rcReload_.left, (float)rcReload_.top,
                               (float)(rcReload_.right - rcReload_.left),
                               (float)(rcReload_.bottom - rcReload_.top)),
                         &sfCenter, &brText);
        }

        g.DrawString(L"\u663e\u793a\u7cfb\u7edf\u684c\u9762\u56fe\u6807", -1, &fBody,
                     RectF((float)kRowLeft, (float)(rcSysIcons_.top - 10), 320.0f, 26.0f),
                     &sf, &brText);
        g.DrawString(L"\u5173\u6389\u4f7f\u7528\u6211\u4eec\u81ea\u5df1\u7684\u684c\u9762\u5c42",
                     -1, &fSub,
                     RectF((float)kRowLeft, (float)(rcSysIcons_.top + 12), 340.0f, 16.0f),
                     &sf, &brSub);
        {
            const bool shown = getSystemIconsShown ? getSystemIconsShown() : false;
            drawToggle(rcSysIcons_, shown);
        }

        // ---- 分隔线：上面是「桌面」，下面是「启动」 ----
        {
            Pen line(C(t.cardBorder), 1.0f);
            g.DrawLine(&line, (float)rcStartDivider_.left, (float)rcStartDivider_.top,
                       (float)rcStartDivider_.right, (float)rcStartDivider_.top);
        }

        g.DrawString(L"\u5f00\u673a\u81ea\u542f", -1, &fBody,
                     RectF((float)kRowLeft, (float)(rcAutoStart_.top - 10), 320.0f, 26.0f),
                     &sf, &brText);
        g.DrawString(L"\u767b\u5f55 Windows \u540e\u81ea\u52a8\u542f\u52a8", -1, &fSub,
                     RectF((float)kRowLeft, (float)(rcAutoStart_.top + 12), 340.0f, 16.0f),
                     &sf, &brSub);
        {
            const bool on = getAutoStart ? getAutoStart() : false;
            drawToggle(rcAutoStart_, on);
        }

        g.DrawString(L"\u9ad8\u4f18\u5148\u7ea7\u8fd0\u884c", -1, &fBody,
                     RectF((float)kRowLeft, (float)(rcHighPriority_.top - 10), 320.0f, 26.0f),
                     &sf, &brText);
        g.DrawString(L"\u684c\u9762\u91cd\u7ed8\u66f4\u8ddf\u624b\uff0c\u6ee1\u8f7d\u65f6\u4e0d\u6613\u6389\u5e27",
                     -1, &fSub,
                     RectF((float)kRowLeft, (float)(rcHighPriority_.top + 12), 340.0f, 16.0f),
                     &sf, &brSub);
        {
            const bool on = getHighPriority ? getHighPriority() : false;
            drawToggle(rcHighPriority_, on);
        }
        break;
    }

    case Page::Appearance: {
        g.DrawString(L"\u5916\u89c2", -1, &fPage, RectF((float)kContentX, 26.0f, 300.0f, 30.0f),
                     &sf, &brText);
        g.DrawString(L"\u76d2\u5b50\u7684\u989c\u8272\u3001\u5f62\u72b6\u4e0e\u901a\u900f\u5ea6",
                     -1, &fSub, RectF((float)kContentX + 2, 56.0f, 460.0f, 18.0f), &sf,
                     &brSub);

        const int c1 = 100;
        drawCard(c1, 316);
        g.DrawString(L"\u76d2\u5b50\u5916\u89c2", -1, &fSection,
                     RectF((float)kContentX + 20, (float)c1 + 14.0f, 260.0f, 22.0f), &sf,
                     &brText);

        g.DrawString(L"\u989c\u8272", -1, &fBody,
                     RectF((float)kRowLeft, (float)(c1 + 44), 80.0f, 26.0f), &sf,
                     &brText);
        for (int i = 0; i < 12; ++i) {
            const RECT& r = swatches_[i];
            const bool sel = (base == kSwatchColors[i]);
            GraphicsPath p;
            AddRound(p, RectF((float)r.left, (float)r.top, 26.0f, 26.0f), 7.0f);
            SolidBrush fill(C(kSwatchColors[i]));
            g.FillPath(&fill, &p);
            if (sel) {
                Pen ring(C(t.accent), 2.0f);
                g.DrawPath(&ring, &p);
            } else {
                Pen pen(C(t.cardBorder), 1.0f);
                g.DrawPath(&pen, &p);
            }
        }

        g.DrawString(L"\u5706\u89d2", -1, &fBody,
                     RectF((float)kRowLeft, (float)rcRadius_.top, 90.0f, 30.0f), &sf,
                     &brText);
        drawSlider(rcRadius_, 0, 28, radius, std::to_wstring(radius));

        g.DrawString(L"\u900f\u660e\u5ea6", -1, &fBody,
                     RectF((float)kRowLeft, (float)rcAlpha_.top, 90.0f, 30.0f), &sf,
                     &brText);
        {
            wchar_t buf[16];
            swprintf_s(buf, L"%d%%", (int)std::lround(alpha * 100.0 / 255.0));
            drawSlider(rcAlpha_, 40, 255, alpha, buf);
        }

        g.DrawString(L"\u5934\u90e8\u9ad8\u5ea6", -1, &fBody,
                     RectF((float)kRowLeft, (float)rcHeaderH_.top, 90.0f, 30.0f), &sf,
                     &brText);
        drawSlider(rcHeaderH_, 28, 72, std::clamp(g_config.headerHeight, 28, 72),
                   std::to_wstring(std::clamp(g_config.headerHeight, 28, 72)) + L" px");

        g.DrawString(L"Win11 \u4e9a\u514b\u529b\u6548\u679c", -1, &fBody,
                     RectF((float)kRowLeft, (float)(rcAcrylic_.top - 10), 300.0f, 26.0f),
                     &sf, &brText);
        g.DrawString(L"\u6a21\u7cca\u80cc\u666f\uff0c\u66f4\u63a5\u8fd1\u7cfb\u7edf\u539f\u751f\u8d28\u611f",
                     -1, &fSub,
                     RectF((float)kRowLeft, (float)(rcAcrylic_.top + 12), 340.0f, 16.0f),
                     &sf, &brSub);
        drawToggle(rcAcrylic_, acrylic);

        const int c2 = 436;
        drawCard(c2, 170);
        g.DrawString(L"\u6807\u9898", -1, &fSection,
                     RectF((float)kContentX + 20, (float)c2 + 14.0f, 200.0f, 22.0f), &sf,
                     &brText);
        g.DrawString(L"\u663e\u793a\u56fe\u6807\u6807\u9898", -1, &fBody,
                     RectF((float)kRowLeft, (float)(rcShowLabels_.top - 10), 300.0f, 26.0f),
                     &sf, &brText);
        g.DrawString(L"\u5173\u95ed\u540e\u684c\u9762\u548c\u76d2\u5b50\u91cc\u90fd\u4e0d\u663e\u793a\u540d\u5b57",
                     -1, &fSub,
                     RectF((float)kRowLeft, (float)(rcShowLabels_.top + 12), 360.0f, 16.0f),
                     &sf, &brSub);
        drawToggle(rcShowLabels_, g_config.showLabels != 0);

        g.DrawString(L"\u663e\u793a\u76d2\u5b50\u6807\u9898", -1, &fBody,
                     RectF((float)kRowLeft, (float)(rcShowFenceTitle_.top - 10), 300.0f, 26.0f),
                     &sf, &brText);
        g.DrawString(L"\u5173\u95ed\u540e\u6807\u9898\u548c\u53f3\u4e0a\u89d2\u7bad\u5934\u90fd\u4e0d\u663e\u793a\uff0c"
                     L"\u9876\u90e8\u53ea\u7559 2px \u60ac\u505c\u533a",
                     -1, &fSub,
                     RectF((float)kRowLeft, (float)(rcShowFenceTitle_.top + 12), 420.0f, 16.0f),
                     &sf, &brSub);
        drawToggle(rcShowFenceTitle_, g_config.showFenceTitle != 0);
        break;
    }

    case Page::Interaction: {
        g.DrawString(L"\u4ea4\u4e92", -1, &fPage, RectF((float)kContentX, 26.0f, 300.0f, 30.0f),
                     &sf, &brText);
        g.DrawString(L"\u9f20\u6807\u4e0e\u76d2\u5b50\u7684\u54cd\u5e94\u65b9\u5f0f", -1, &fSub,
                     RectF((float)kContentX + 2, 56.0f, 460.0f, 18.0f), &sf, &brSub);

        const int c1 = 100;
        drawCard(c1, 200);
        g.DrawString(L"\u76d2\u5b50\u884c\u4e3a", -1, &fSection,
                     RectF((float)kContentX + 20, (float)c1 + 14.0f, 260.0f, 22.0f), &sf,
                     &brText);

        g.DrawString(L"\u9f20\u6807\u79fb\u5230\u5934\u90e8\u81ea\u52a8\u6253\u5f00", -1, &fBody,
                     RectF((float)kRowLeft, (float)(rcHoverExpand_.top - 10), 360.0f, 26.0f),
                     &sf, &brText);
        g.DrawString(L"\u6536\u8d77\u65f6\u79fb\u5165\u5934\u90e8\u5c55\u5f00\uff0c"
                     L"\u79fb\u51fa\u6574\u4e2a\u76d2\u5b50\u81ea\u52a8\u6536\u8d77",
                     -1, &fSub,
                     RectF((float)kRowLeft, (float)(rcHoverExpand_.top + 12), 420.0f, 16.0f),
                     &sf, &brSub);
        drawToggle(rcHoverExpand_, g_config.autoExpandOnHover != 0);

        g.DrawString(L"\u6392\u5e8f\u65f6\u64ad\u653e\u7ffb\u8f6c\u52a8\u753b", -1, &fBody,
                     RectF((float)kRowLeft, (float)(rcSortAnim_.top - 10), 360.0f, 26.0f),
                     &sf, &brText);
        g.DrawString(L"\u6539\u6392\u5e8f\u65b9\u5f0f\u65f6\u56fe\u6807\u4f1a\u7ed5\u7ad6\u8f74"
                     L"\u7ffb\u8fc7\u53bb\u91cd\u6392",
                     -1, &fSub,
                     RectF((float)kRowLeft, (float)(rcSortAnim_.top + 12), 420.0f, 16.0f),
                     &sf, &brSub);
        drawToggle(rcSortAnim_, g_config.sortAnim != 0);

        g.DrawString(L"\u5206\u7ec4\u6807\u7b7e\u79fb\u5165\u5c31\u5207\u6362", -1, &fBody,
                     RectF((float)kRowLeft, (float)(rcTabHover_.top - 10), 360.0f, 26.0f),
                     &sf, &brText);
        g.DrawString(L"\u5173\u95ed\u540e\u9700\u8981\u70b9\u51fb\u5934\u90e8\u7684\u6807\u7b7e"
                     L"\u624d\u80fd\u5207\u6362\u5206\u7ec4",
                     -1, &fSub,
                     RectF((float)kRowLeft, (float)(rcTabHover_.top + 12), 420.0f, 16.0f),
                     &sf, &brSub);
        drawToggle(rcTabHover_, g_config.switchTabOnHover != 0);
        break;
    }

    case Page::Rules:
        PaintRulesPage(g, t);
        break;

    case Page::About: {
        g.DrawString(L"\u5173\u4e8e", -1, &fPage, RectF((float)kContentX, 26.0f, 300.0f, 30.0f),
                     &sf, &brText);
        g.DrawString(L"Meow Desk \u00b7 \u8f7b\u91cf\u684c\u9762\u5206\u533a\u5de5\u5177", -1,
                     &fSub, RectF((float)kContentX + 2, 56.0f, 460.0f, 18.0f), &sf, &brSub);

        const int c1 = 100;
        drawCard(c1, 220);

        // app 图标 + 右侧说明文字（logo 取自 exe 资源里的 128px PNG，失败就只显示文字）
        const int logoPx = 64;
        const int textX = kContentX + 20 + logoPx + 16;
        if (Bitmap* logo = AppLogoBitmap()) {
            // 用 HighQualityBicubic 缩到 64px：直接 DrawImage 走的是最近邻似的
            // 默认插值，边缘一圈会发毛。
            InterpolationMode old = g.GetInterpolationMode();
            g.SetInterpolationMode(InterpolationModeHighQualityBicubic);
            g.SetPixelOffsetMode(PixelOffsetModeHighQuality);
            g.DrawImage(logo, RectF((float)kContentX + 20, (float)c1 + 16.0f, (float)logoPx,
                                    (float)logoPx));
            g.SetInterpolationMode(old);
        }

        g.DrawString(L"Meow Desk", -1, &fSection,
                     RectF((float)textX, (float)c1 + 18.0f, 300.0f, 24.0f), &sf, &brText);

        Font fAbout(&family, 12.5f, FontStyleRegular, UnitPixel);
        const wchar_t* lines[] = {
            L"\u7248\u672c 1.0.0 \u00b7 C++ / Win32 \u539f\u751f\u5b9e\u73b0",
            L"\u684c\u9762\u56fe\u6807\u7531\u672c\u7a0b\u5e8f\u81ea\u7ed8\uff0c"
            L"\u6587\u4ef6\u59cb\u7ec8\u7559\u5728\u684c\u9762\u4e0a",
            L"\u9000\u51fa\u540e\u771f\u5b9e\u684c\u9762\u81ea\u52a8\u6062\u590d\u539f\u6837",
        };
        // 三行说明和 logo 挤在同一块高度里（logo 16..80），所以起点从 c1+46 起
        float ly = (float)c1 + 46.0f;
        for (const wchar_t* s : lines) {
            g.DrawString(s, -1, &fAbout, RectF((float)textX, ly, 460.0f, 20.0f), &sf, &brSub);
            ly += 20.0f;
        }

        g.DrawString(L"\u6570\u636e\u6587\u4ef6", -1, &fBody,
                     RectF((float)kRowLeft, (float)c1 + 120.0f, 200.0f, 22.0f), &sf, &brText);
        g.DrawString(AppConfig::File().c_str(), -1, &fSub,
                     RectF((float)kRowLeft, (float)c1 + 146.0f, 520.0f, 18.0f), &sf, &brSub);

        {
            GraphicsPath p;
            AddRound(p, RectF((float)rcOpenFolder_.left, (float)rcOpenFolder_.top,
                              (float)(rcOpenFolder_.right - rcOpenFolder_.left),
                              (float)(rcOpenFolder_.bottom - rcOpenFolder_.top)),
                     6.0f);
            SolidBrush fill(C(t.cardBorder));
            g.FillPath(&fill, &p);
            Pen pen(C(t.cardBorder), 1.0f);
            g.DrawPath(&pen, &p);
            // 现在存的是 meow-desk.db，说「配置文件夹」已经不准确了
            g.DrawString(L"\u6253\u5f00\u6570\u636e\u6587\u4ef6\u5939", -1, &fBody,
                         RectF((float)rcOpenFolder_.left, (float)rcOpenFolder_.top,
                               (float)(rcOpenFolder_.right - rcOpenFolder_.left),
                               (float)(rcOpenFolder_.bottom - rcOpenFolder_.top)),
                         &sfCenter, &brText);
        }
        break;
    }

    default:
        break;
    }
}

// ---------------------------------------------------------------------------
// 绘制：整理规则页
//
// 布局取自 my-desktop 的「整理规则」：说明 → 自动整理开关 → 规则列表 →
// 添加 / 恢复默认 / 立即整理。每行左边是规则名（=要归入的盒子名），
// 右边是扩展名列表；文件夹规则和兜底规则没有扩展名，直接写死说明文字。
// ---------------------------------------------------------------------------
void SettingsWindow::PaintRulesPage(Graphics& g, const SettingsTheme& t) {
    FontFamily family(L"Microsoft YaHei UI");
    Font fPage(&family, 19.0f, FontStyleBold, UnitPixel);
    Font fSection(&family, 14.5f, FontStyleBold, UnitPixel);
    Font fBody(&family, 13.5f, FontStyleRegular, UnitPixel);
    Font fSub(&family, 12.0f, FontStyleRegular, UnitPixel);

    StringFormat sf;
    sf.SetLineAlignment(StringAlignmentCenter);
    sf.SetFormatFlags(StringFormatFlagsNoWrap);
    StringFormat sfCenter;
    sfCenter.SetAlignment(StringAlignmentCenter);
    sfCenter.SetLineAlignment(StringAlignmentCenter);

    SolidBrush brText(C(t.text));
    SolidBrush brSub(C(t.sub));

    g.DrawString(L"\u6574\u7406\u89c4\u5219", -1, &fPage,
                 RectF((float)kContentX, 26.0f, 300.0f, 30.0f), &sf, &brText);
    // 这段说明比较长，交给 GDI+ 自动换行（这里不能带 NoWrap）
    StringFormat wrap;
    wrap.SetLineAlignment(StringAlignmentNear);
    g.DrawString(
        L"\u4e00\u952e\u6574\u7406\u65f6\uff0c\u684c\u9762\u4e0a\u7684\u56fe\u6807\u6309\u4e0b\u5217"
        L"\u89c4\u5219\u4ece\u4e0a\u5230\u4e0b\u5339\u914d\uff0c\u5f52\u5165\u540c\u540d\u5206\u533a"
        L"\uff08\u4e0d\u5b58\u5728\u65f6\u81ea\u52a8\u521b\u5efa\uff09\u3002\u53ea\u662f\u5f52\u7c7b"
        L"\u663e\u793a\uff0c\u6587\u4ef6\u4ecd\u7559\u5728\u684c\u9762\u6587\u4ef6\u5939\u91cc\u3002"
        L"\u6269\u5c55\u540d\u4e4b\u95f4\u7528\u5206\u53f7\u5206\u9694\u3002",
        -1, &fSub,
        RectF((float)kContentX + 2, 54.0f, (float)kCardW - 20.0f, 40.0f), &wrap, &brSub);

    auto drawCard = [&](int y, int ch) {
        GraphicsPath p;
        AddRound(p, RectF((float)kContentX, (float)y, (float)kCardW, (float)ch), 10.0f);
        SolidBrush fill(C(t.card));
        g.FillPath(&fill, &p);
        Pen pen(C(t.cardBorder), 1.0f);
        g.DrawPath(&pen, &p);
    };

    auto drawToggle = [&](const RECT& r, bool on) {
        const float tw = (float)(r.right - r.left);
        const float th = (float)(r.bottom - r.top);
        GraphicsPath p;
        AddRound(p, RectF((float)r.left, (float)r.top, tw, th), th * 0.5f);
        SolidBrush track(C(on ? t.accent : t.track));
        g.FillPath(&track, &p);
        if (!on) {
            Pen pen(C(dark_ ? RGB(90, 90, 90) : RGB(200, 200, 200)), 1.0f);
            g.DrawPath(&pen, &p);
        }
        const float d = th - 8.0f;
        const float cx = on ? (float)r.right - 4.0f - d * 0.5f : (float)r.left + 4.0f + d * 0.5f;
        const float cy = (float)r.top + th * 0.5f;
        SolidBrush knob(C(on ? RGB(255, 255, 255) : RGB(120, 120, 120)));
        g.FillEllipse(&knob, cx - d * 0.5f, cy - d * 0.5f, d, d);
    };

    // ---- 自动整理 ----
    drawCard(100, 84);
    g.DrawString(L"\u81ea\u52a8\u6574\u7406\u65b0\u6587\u4ef6", -1, &fBody,   // 自动整理新文件
                 RectF((float)kRowLeft, (float)(rcAutoOrganize_.top - 12), 320.0f, 26.0f), &sf,
                 &brText);
    g.DrawString(L"\u684c\u9762\u4e0a\u65b0\u51fa\u73b0\u7684\u6587\u4ef6\uff08\u5982\u521a"
                 L"\u4e0b\u8f7d\u3001\u521a\u4fdd\u5b58\u7684\uff09\u51e0\u79d2\u540e\u81ea\u52a8"
                 L"\u5f52\u5165\u5206\u533a",
                 -1, &fSub,
                 RectF((float)kRowLeft, (float)(rcAutoOrganize_.top + 12), 520.0f, 16.0f), &sf,
                 &brSub);
    drawToggle(rcAutoOrganize_, g_config.autoOrganize != 0);

    // ---- 规则列表 ----
    g.DrawString(L"\u89c4\u5219\u5217\u8868", -1, &fSection,
                 RectF((float)kContentX, 190.0f, 240.0f, 22.0f), &sf, &brText);

    for (size_t i = 0; i < ruleRows_.size(); ++i) {
        const RuleRow& row = ruleRows_[i];
        const int idx = row.ruleIndex;
        if (idx < 0 || idx >= (int)g_config.rules.size()) continue;
        const OrganizeRule& r = g_config.rules[idx];

        GraphicsPath p;
        AddRound(p,
                 RectF((float)(rcRulesList_.left), (float)(row.name.top - 7),
                       (float)(rcRulesList_.right - rcRulesList_.left), 40.0f),
                 8.0f);
        SolidBrush fill(C(t.card));
        g.FillPath(&fill, &p);
        Pen pen(C(t.cardBorder), 1.0f);
        g.DrawPath(&pen, &p);

        // 没有扩展名的两种规则：把说明文字画在扩展名那一栏
        if (r.matchFolders || r.isFallback) {
            const wchar_t* hint = r.matchFolders != 0
                                      ? L"\u684c\u9762\u4e0a\u7684\u6240\u6709\u6587\u4ef6\u5939"
                                      : // 桌面上的所有文件夹
                                      L"\u5176\u4ed6\u89c4\u5219\u90fd\u6ca1\u6709\u5339\u914d"
                                      L"\u5230\u7684\u9879\u76ee";   // 其他规则都没有匹配到的项目
            g.DrawString(hint, -1, &fSub,
                         RectF((float)row.exts.left + 4, (float)row.exts.top,
                               (float)(row.exts.right - row.exts.left - 4),
                               (float)(row.exts.bottom - row.exts.top)),
                         &sf, &brSub);
        }

        // 删除按钮（垃圾桶简笔）
        {
            const RECT& d = row.del;
            const bool hot = (hot_ == ID_RULE_DEL + (int)i);
            if (hot) {
                GraphicsPath bp;
                AddRound(bp,
                         RectF((float)d.left, (float)d.top, (float)(d.right - d.left),
                               (float)(d.bottom - d.top)),
                         6.0f);
                SolidBrush hb(CA(t.accent, 26));
                g.FillPath(&hb, &bp);
            }
            const float cx = (float)(d.left + d.right) * 0.5f;
            const float cy = (float)(d.top + d.bottom) * 0.5f;
            Pen pen(C(t.sub), 1.4f);
            pen.SetStartCap(LineCapRound);
            pen.SetEndCap(LineCapRound);
            g.DrawLine(&pen, cx - 5.0f, cy - 4.0f, cx + 5.0f, cy - 4.0f);
            g.DrawLine(&pen, cx - 3.0f, cy - 4.0f, cx - 3.0f, cy + 5.0f);
            g.DrawLine(&pen, cx + 3.0f, cy - 4.0f, cx + 3.0f, cy + 5.0f);
            g.DrawLine(&pen, cx - 4.0f, cy + 5.0f, cx + 4.0f, cy + 5.0f);
            g.DrawLine(&pen, cx, cy - 6.0f, cx, cy - 4.0f);
        }
    }

    if ((int)g_config.rules.size() > rulesScroll_ + (int)ruleRows_.size()) {
        // 有被卷上去/滚下去的行，给个提示（没有滚动条，靠滚轮）
        g.DrawString(L"\u6eda\u8f6e\u7ffb\u9875", -1, &fSub,
                     RectF((float)rcRulesList_.right - 100.0f, 190.0f, 100.0f, 22.0f),
                     StringFormat::GenericDefault(), &brSub);
    }

    // ---- 底部按钮 ----
    auto drawButton = [&](const RECT& r, const wchar_t* text, bool primary) {
        GraphicsPath p;
        AddRound(p,
                 RectF((float)r.left, (float)r.top, (float)(r.right - r.left),
                       (float)(r.bottom - r.top)),
                 6.0f);
        SolidBrush fill(primary ? C(t.accent) : C(t.cardBorder));
        g.FillPath(&fill, &p);
        Pen pen(C(primary ? t.accent : t.cardBorder), 1.0f);
        g.DrawPath(&pen, &p);
        SolidBrush white(C(RGB(255, 255, 255)));
        g.DrawString(text, -1, &fBody,
                     RectF((float)r.left, (float)r.top, (float)(r.right - r.left),
                           (float)(r.bottom - r.top)),
                     &sfCenter, primary ? &white : &brText);
    };
    drawButton(rcAddRule_, L"\u6dfb\u52a0\u89c4\u5219", false);          // 添加规则
    drawButton(rcResetRules_, L"\u6062\u590d\u9ed8\u8ba4\u89c4\u5219", false);  // 恢复默认规则
    drawButton(rcOrganizeNow_, L"\u7acb\u5373\u6574\u7406\u684c\u9762\u2026", true);  // 立即整理桌面…
}

void SettingsWindow::Paint() {
    if (!hwnd_) return;
    RECT cr{};
    GetClientRect(hwnd_, &cr);
    const int w = cr.right, h = cr.bottom;
    if (!EnsureBackbuffer(w, h)) return;

    const SettingsTheme t = dark_ ? DarkTheme() : LightTheme();

    // 用不带 alpha 的 32bpp RGB：GDI+ 只在这种格式上真正启用 ClearType
    Bitmap target(w, h, w * 4, PixelFormat32bppRGB, reinterpret_cast<BYTE*>(bits_));
    Graphics g(&target);
    g.SetSmoothingMode(SmoothingModeAntiAlias);
    g.SetPixelOffsetMode(PixelOffsetModeHighQuality);
    // 设置窗口是**不透明**的（GDI+ 画进 DIB 再 BitBlt 到窗口），所以可以用 ClearType：
    // 灰度抗锯齿（TextRenderingHintAntiAlias）会把汉字笔画糊成半透明灰边，看着"发虚"。
    // 注意：桌面层/盒子是半透明的分层窗口，那边不能用 ClearType，否则边缘会出彩色杂边。
    g.SetTextRenderingHint(TextRenderingHintClearTypeGridFit);
    g.Clear(C(t.bg));

    PaintNav(g, t);
    PaintPage(g, t);

    g.Flush(FlushIntentionSync);

    HDC dc = GetDC(hwnd_);
    BitBlt(dc, 0, 0, w, h, memDC_, 0, 0, SRCCOPY);
    ReleaseDC(hwnd_, dc);
}

// ---------------------------------------------------------------------------
// 交互
// ---------------------------------------------------------------------------
void SettingsWindow::Toggle(int id) {
    switch (id) {
    case ID_ACRYLIC: {
        const int nv = g_config.fences.empty() ? 1 : (g_config.fences[0].acrylic ? 0 : 1);
        for (auto& f : g_config.fences) f.acrylic = nv;
        break;
    }
    case ID_SHADOW: g_config.textShadow = g_config.textShadow ? 0 : 1; break;
    case ID_ARROW: g_config.shortcutArrow = g_config.shortcutArrow ? 0 : 1; break;
    case ID_SHOW_LABELS: g_config.showLabels = g_config.showLabels ? 0 : 1; break;
    case ID_SHOW_FENCE_TITLE:
        g_config.showFenceTitle = g_config.showFenceTitle ? 0 : 1;
        break;
    case ID_AUTO_ORGANIZE:
        g_config.autoOrganize = g_config.autoOrganize ? 0 : 1;
        break;
    case ID_HOVER_EXPAND:
        g_config.autoExpandOnHover = g_config.autoExpandOnHover ? 0 : 1;
        break;
    case ID_SORT_ANIM:
        g_config.sortAnim = g_config.sortAnim ? 0 : 1;
        break;
    case ID_TAB_HOVER:
        g_config.switchTabOnHover = g_config.switchTabOnHover ? 0 : 1;
        break;
    case ID_SYSICONS:
        if (setSystemIconsShown) {
            const bool shown = getSystemIconsShown ? getSystemIconsShown() : false;
            setSystemIconsShown(!shown);
        }
        break;
    case ID_AUTOSTART:
        if (setAutoStart) {
            const bool on = getAutoStart ? getAutoStart() : false;
            setAutoStart(!on);
        }
        break;
    case ID_HIGHPRIORITY:
        if (setHighPriority) {
            const bool on = getHighPriority ? getHighPriority() : false;
            setHighPriority(!on);
        }
        break;
    default:
        return;
    }
    NotifyChanged();
    g_config.Save();
    Paint();
}

// ---------------------------------------------------------------------------
// 整理规则：输入行
// ---------------------------------------------------------------------------
int SettingsWindow::RulesVisibleRows() const {
    const int step = 46;   // 行高 40 + 间隙 6
    const int h = rcRulesList_.bottom - rcRulesList_.top;
    const int n = h / step;
    return n > 0 ? n : 1;
}

void SettingsWindow::DestroyRuleControls() {
    for (auto& r : ruleRows_) {
        if (r.nameEdit) { DestroyWindow(r.nameEdit); r.nameEdit = nullptr; }
        if (r.extEdit) { DestroyWindow(r.extEdit); r.extEdit = nullptr; }
    }
    ruleRows_.clear();
}

// 输入框里的字是「正在编辑」的真相：重建/切页/关窗前必须收回配置，
// 否则用户刚打的字会随着控件销毁一起消失。
void SettingsWindow::SyncRulesFromControls() {
    for (auto& row : ruleRows_) {
        if (row.ruleIndex < 0 || row.ruleIndex >= (int)g_config.rules.size()) continue;
        OrganizeRule& r = g_config.rules[row.ruleIndex];
        if (row.nameEdit) {
            wchar_t buf[256]{};
            GetWindowTextW(row.nameEdit, buf, 256);
            r.name = buf;
        }
        if (row.extEdit) {
            wchar_t buf[2048]{};
            GetWindowTextW(row.extEdit, buf, 2048);
            r.exts = buf;
        }
    }
}

void SettingsWindow::BuildRuleControls() {
    if (!hwnd_) return;
    SyncRulesFromControls();
    DestroyRuleControls();
    if (page_ != Page::Rules) return;

    const int total = (int)g_config.rules.size();
    if (rulesScroll_ > total - 1) rulesScroll_ = total - 1;
    if (rulesScroll_ < 0) rulesScroll_ = 0;

    if (!ruleFont_) {
        // 直接用 LOGFONT 建字体：GDI+ 的 Font 要拿到 Graphics 才能导出 LOGFONT，
        // 这里只需要「13px 微软雅黑」，手写一份更省事
        LOGFONTW lf{};
        lf.lfHeight = -13;
        lf.lfWeight = FW_NORMAL;
        lf.lfCharSet = DEFAULT_CHARSET;
        lf.lfQuality = CLEARTYPE_QUALITY;
        wcscpy_s(lf.lfFaceName, L"Microsoft YaHei UI");
        ruleFont_ = CreateFontIndirectW(&lf);
    }

    const int vis = RulesVisibleRows();
    for (int i = 0; i < vis; ++i) {
        const int idx = rulesScroll_ + i;
        if (idx >= total) break;
        const OrganizeRule& r = g_config.rules[idx];
        RuleRow row;
        row.ruleIndex = idx;
        const int y = rcRulesList_.top + i * 46;
        row.name = RECT{rcRulesList_.left + 10, y + 7, rcRulesList_.left + 152, y + 33};
        row.exts = RECT{rcRulesList_.left + 162, y + 7, rcRulesList_.right - 44, y + 33};
        row.del = RECT{rcRulesList_.right - 34, y + 8, rcRulesList_.right - 10, y + 32};

        const DWORD baseStyle = WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL;
        row.nameEdit = CreateWindowExW(
            0, L"EDIT", r.name.c_str(), baseStyle, row.name.left, row.name.top,
            row.name.right - row.name.left, row.name.bottom - row.name.top, hwnd_,
            (HMENU)(INT_PTR)(ID_RULE_EDIT + i * 2), nullptr, GetModuleHandleW(nullptr));
        if (row.nameEdit) SendMessageW(row.nameEdit, WM_SETFONT, (WPARAM)ruleFont_, TRUE);

        if (!r.matchFolders && !r.isFallback) {
            row.extEdit = CreateWindowExW(
                0, L"EDIT", r.exts.c_str(), baseStyle, row.exts.left, row.exts.top,
                row.exts.right - row.exts.left, row.exts.bottom - row.exts.top, hwnd_,
                (HMENU)(INT_PTR)(ID_RULE_EDIT + i * 2 + 1), nullptr, GetModuleHandleW(nullptr));
            if (row.extEdit) SendMessageW(row.extEdit, WM_SETFONT, (WPARAM)ruleFont_, TRUE);
        }
        ruleRows_.push_back(row);
    }
}

void SettingsWindow::ScrollRules(int delta) {
    const int total = (int)g_config.rules.size();
    const int vis = RulesVisibleRows();
    const int maxScroll = std::max(0, total - vis);
    const int next = std::clamp(rulesScroll_ + delta, 0, maxScroll);
    if (next == rulesScroll_) return;
    rulesScroll_ = next;
    BuildRuleControls();   // 重建（内部会先把输入框里的字收回配置）
    Paint();
}

LRESULT SettingsWindow::WndProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_ERASEBKGND:
        return 1;

    case WM_PAINT: {
        PAINTSTRUCT ps{};
        BeginPaint(h, &ps);
        Paint();
        EndPaint(h, &ps);
        return 0;
    }

    // 规则行的输入框用系统 EDIT：底色/字色跟卡片走，不然白底会跳出面板
    case WM_CTLCOLOREDIT: {
        HDC dc = reinterpret_cast<HDC>(wp);
        const SettingsTheme t = dark_ ? DarkTheme() : LightTheme();
        SetTextColor(dc, t.text);
        SetBkColor(dc, t.card);
        if (!ruleBrush_) ruleBrush_ = CreateSolidBrush(t.card);
        return reinterpret_cast<LRESULT>(ruleBrush_);
    }

    case WM_COMMAND: {
        const int id = LOWORD(wp);
        if (id >= ID_RULE_EDIT && HIWORD(wp) == EN_CHANGE) {
            const int row = (id - ID_RULE_EDIT) / 2;
            const bool isExt = ((id - ID_RULE_EDIT) % 2) != 0;
            if (row >= 0 && row < (int)ruleRows_.size()) {
                const int idx = ruleRows_[row].ruleIndex;
                if (idx >= 0 && idx < (int)g_config.rules.size()) {
                    wchar_t buf[2048]{};
                    HWND ed = isExt ? ruleRows_[row].extEdit : ruleRows_[row].nameEdit;
                    if (ed) GetWindowTextW(ed, buf, 2048);
                    if (isExt) g_config.rules[idx].exts = buf;
                    else g_config.rules[idx].name = buf;
                    // 每敲一个字就写库太重（FULL 同步），攒一下再存
                    SetTimer(h, 9, 500, nullptr);
                }
            }
            return 0;
        }
        break;
    }

    case WM_TIMER:
        if (wp == 9) {
            KillTimer(h, 9);
            g_config.Save();
            return 0;
        }
        break;

    case WM_MOUSEWHEEL: {
        if (page_ == Page::Rules) {
            const int delta = GET_WHEEL_DELTA_WPARAM(wp);
            ScrollRules(delta < 0 ? 1 : -1);
            return 0;
        }
        break;
    }

    case WM_LBUTTONDOWN: {
        POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        const int id = HitTest(pt);

        if (id >= ID_NAV0 && id < ID_NAV0 + 5) {
            const Page np = (Page)(id - ID_NAV0);
            if (np != page_) {
                if (page_ == Page::Rules) SyncRulesFromControls();   // 先收字再切页
                page_ = np;
                if (page_ == Page::Rules) {
                    rulesScroll_ = 0;
                    BuildRuleControls();
                } else {
                    DestroyRuleControls();
                }
            }
            hot_ = id;
            Paint();
            return 0;
        }
        if (id >= ID_SWATCH0 && id < ID_SWATCH0 + 12) {
            const COLORREF c = kSwatchColors[id - ID_SWATCH0];
            for (auto& f : g_config.fences) f.color = (int)c;
            NotifyChanged();
            g_config.Save();
            Paint();
            return 0;
        }
        if (id == ID_RELOAD) {
            if (onReloadIcons) onReloadIcons();
            Paint();
            return 0;
        }
        if (id == ID_OPENFOLDER) {
            ShellExecuteW(nullptr, L"open", AppConfig::Dir().c_str(), nullptr, nullptr,
                          SW_SHOWNORMAL);
            return 0;
        }
        if (id == ID_ACRYLIC || id == ID_SHADOW || id == ID_ARROW || id == ID_SHOW_LABELS ||
            id == ID_HOVER_EXPAND || id == ID_SORT_ANIM || id == ID_SYSICONS ||
            id == ID_TAB_HOVER || id == ID_SHOW_FENCE_TITLE || id == ID_AUTO_ORGANIZE ||
            id == ID_AUTOSTART || id == ID_HIGHPRIORITY) {
            Toggle(id);
            return 0;
        }
        // ---- 整理规则页的三个按钮 ----
        if (id == ID_ADD_RULE) {
            SyncRulesFromControls();
            OrganizeRule r;
            r.name = L"\u65b0\u89c4\u5219";   // 新规则
            r.exts = L".ext";
            // 兜底规则永远在最后，新规则插在它前面
            auto it = std::find_if(g_config.rules.begin(), g_config.rules.end(),
                                   [](const OrganizeRule& x) { return x.isFallback != 0; });
            const int at = (int)(it - g_config.rules.begin());
            g_config.rules.insert(g_config.rules.begin() + at, r);
            // 滚到能看到新规则的那一行
            const int vis = RulesVisibleRows();
            if (at >= rulesScroll_ + vis) rulesScroll_ = at - vis + 1;
            BuildRuleControls();
            g_config.Save();
            Paint();
            return 0;
        }
        if (id == ID_RESET_RULES) {
            DestroyRuleControls();
            g_config.rules = OrganizeRulesDefault();
            rulesScroll_ = 0;
            BuildRuleControls();
            g_config.Save();
            Paint();
            return 0;
        }
        if (id == ID_ORGANIZE_NOW) {
            SyncRulesFromControls();
            g_config.Save();
            if (onOrganize) onOrganize();
            Paint();
            return 0;
        }
        if (id >= ID_RULE_DEL && id < ID_RULE_DEL + (int)ruleRows_.size()) {
            const int row = id - ID_RULE_DEL;
            const int idx = ruleRows_[row].ruleIndex;
            SyncRulesFromControls();
            if (idx >= 0 && idx < (int)g_config.rules.size()) {
                g_config.rules.erase(g_config.rules.begin() + idx);
                BuildRuleControls();
                g_config.Save();
                Paint();
            }
            return 0;
        }
        if (id == ID_RADIUS || id == ID_ALPHA || id == ID_HEADER_H) {
            sliderDrag_ = id;
            SetCapture(h);
            SendMessageW(h, WM_MOUSEMOVE, 0, lp);   // 立刻按点击位置设值
            return 0;
        }
        return 0;
    }

    case WM_MOUSEMOVE: {
        POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        if (sliderDrag_ >= 0) {
            const RECT& row = (sliderDrag_ == ID_RADIUS) ? rcRadius_
                              : (sliderDrag_ == ID_ALPHA) ? rcAlpha_
                                                          : rcHeaderH_;
            RECT tr = TrackRect(row);
            int lo = 0, hi = 100;
            if (sliderDrag_ == ID_RADIUS) { lo = 0; hi = 28; }
            else if (sliderDrag_ == ID_ALPHA) { lo = 40; hi = 255; }
            else { lo = 28; hi = 72; }

            const int span = tr.right - tr.left;
            int v = lo;
            if (span > 0) {
                double t01 = (double)(pt.x - tr.left) / (double)span;
                t01 = std::clamp(t01, 0.0, 1.0);
                v = lo + (int)std::lround(t01 * (hi - lo));
            }
            if (sliderDrag_ == ID_RADIUS) {
                for (auto& f : g_config.fences) f.radius = v;
            } else if (sliderDrag_ == ID_ALPHA) {
                for (auto& f : g_config.fences) f.alpha = v;
            } else {
                g_config.headerHeight = v;
            }
            if (onChanged) onChanged();
            Paint();
            return 0;
        }
        const int hot = HitTest(pt);
        if (hot != hot_) {
            hot_ = hot;
            Paint();
        }
        return 0;
    }

    case WM_LBUTTONUP:
        if (sliderDrag_ >= 0) {
            sliderDrag_ = -1;
            if (GetCapture() == h) ReleaseCapture();
            g_config.Save();
            return 0;
        }
        return 0;

    case WM_SETCURSOR:
        if (LOWORD(lp) == HTCLIENT) {
            POINT pt{};
            GetCursorPos(&pt);
            ScreenToClient(h, &pt);
            const int id = HitTest(pt);
            if (id >= 0) {
                SetCursor(LoadCursorW(nullptr, IDC_HAND));
                return TRUE;
            }
            SetCursor(LoadCursorW(nullptr, IDC_ARROW));
            return TRUE;
        }
        break;

    case WM_CLOSE:
        Hide();
        return 0;

    case WM_DESTROY:
        return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

bool SettingsWindow::Create(HWND owner) {
    EnsureGdiplus();
    owner_ = owner;
    dark_ = !AppsUseLightTheme();

    HINSTANCE inst = GetModuleHandleW(nullptr);
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = WndProcStatic;
    wc.hInstance = inst;
    wc.lpszClassName = kClass;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    RegisterClassExW(&wc);

    // WS_MINIMIZEBOX：标题栏要有最小化按钮。少了它 Win11 只画「关闭」一个按钮。
    // WS_CLIPCHILDREN：整理规则页的输入框是真的子窗口，面板是「画满整张位图再 BitBlt」，
    // 没有这个样式时每次重绘都会把输入框那块一起刷掉（和搜索面板同一个坑）。
    const DWORD kStyle =
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX | WS_CLIPCHILDREN;
    RECT wr{0, 0, kClientW, kClientH};
    AdjustWindowRectEx(&wr, kStyle, FALSE, WS_EX_APPWINDOW);
    const int w = wr.right - wr.left;
    const int h = wr.bottom - wr.top;

    // 居中到「光标所在的那块屏」：以前用主屏尺寸算，副屏上点开会跑到主屏去
    const RECT wa = ScreenUtil::WorkAreaForCursor();
    int sx = (int)wa.left + ((int)(wa.right - wa.left) - w) / 2;
    int sy = (int)wa.top + ((int)(wa.bottom - wa.top) - h) / 2;

    hwnd_ = CreateWindowExW(WS_EX_APPWINDOW, kClass, L"Meow Desk \u8bbe\u7f6e",
                            kStyle,
                            sx, sy, w, h, owner, nullptr, inst, this);
    if (!hwnd_) return false;

    // 窗口图标（标题栏 + Alt-Tab）：和 exe / 托盘同一份资源。
    // 变量名别叫 small / big —— rpcndr.h 里有 `#define small char`，
    // `HICON small` 会被替换成 `HICON char`，报错还只指向这一行。
    {
        const int cx = GetSystemMetrics(SM_CXSMICON);
        const int cy = GetSystemMetrics(SM_CYICON);
        if (HICON icBig = LoadAppIcon(cy)) {
            SendMessageW(hwnd_, WM_SETICON, ICON_BIG, (LPARAM)icBig);
        }
        if (HICON icSmall = LoadAppIcon(cx)) {
            SendMessageW(hwnd_, WM_SETICON, ICON_SMALL, (LPARAM)icSmall);
        }
    }

    // Win11：圆角 + 跟随系统的深浅色标题栏
    DWORD corner = 2;  // DWMWCP_ROUND
    DwmSetWindowAttribute(hwnd_, 33 /*DWMWA_WINDOW_CORNER_PREFERENCE*/, &corner,
                          sizeof(corner));
    BOOL useDark = dark_ ? TRUE : FALSE;
    DwmSetWindowAttribute(hwnd_, 20 /*DWMWA_USE_IMMERSIVE_DARK_MODE*/, &useDark,
                          sizeof(useDark));

    Layout();
    return true;
}

void SettingsWindow::Show() {
    if (!hwnd_) return;
    dark_ = !AppsUseLightTheme();
    if (ruleBrush_) { DeleteObject(ruleBrush_); ruleBrush_ = nullptr; }   // 主题可能变了
    ShowWindow(hwnd_, SW_SHOWNORMAL);
    SetForegroundWindow(hwnd_);
    // 规则页的输入框是懒创建的：每次显示都重建一遍，顺便把配置里的最新内容带进来
    if (page_ == Page::Rules) BuildRuleControls();
    Paint();
}

void SettingsWindow::Hide() {
    if (!hwnd_) return;
    // 输入框里的字还没提交到配置，隐藏前先收走
    if (page_ == Page::Rules) {
        SyncRulesFromControls();
        g_config.Save();
        DestroyRuleControls();
    }
    ShowWindow(hwnd_, SW_HIDE);
}

void SettingsWindow::Destroy() {
    DestroyRuleControls();
    if (ruleFont_) { DeleteObject(ruleFont_); ruleFont_ = nullptr; }
    if (ruleBrush_) { DeleteObject(ruleBrush_); ruleBrush_ = nullptr; }
    if (hwnd_) {
        SetWindowLongPtrW(hwnd_, GWLP_USERDATA, 0);
        DestroyWindow(hwnd_);
        hwnd_ = nullptr;
    }
    if (dib_) { DeleteObject(dib_); dib_ = nullptr; }
    if (memDC_) { DeleteDC(memDC_); memDC_ = nullptr; }
    bits_ = nullptr;
    bufW_ = bufH_ = 0;
}

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
#include "icon_render.h"

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
    ID_SWATCH0 = 0,   // 0..7
    ID_RADIUS = 20,
    ID_ALPHA = 21,
    ID_ACRYLIC = 22,
    ID_SHADOW = 23,
    ID_ARROW = 24,
    ID_HEADER_H = 25,
    ID_SHOW_LABELS = 26,
    ID_HOVER_EXPAND = 27,
    ID_RELOAD = 40,
    ID_SYSICONS = 41,
    ID_OPENFOLDER = 42,
    ID_NAV0 = 60,     // 60..63 = 导航项
};

// Fluent 调色
const COLORREF kSwatchColors[8] = {
    RGB(250, 251, 254), RGB(232, 240, 254), RGB(224, 247, 250), RGB(232, 245, 233),
    RGB(255, 248, 225), RGB(252, 232, 230), RGB(243, 233, 245), RGB(224, 224, 224),
};

const wchar_t* kNavNames[4] = {
    L"\u5e38\u89c4",      // 常规
    L"\u5916\u89c2",      // 外观
    L"\u4ea4\u4e92",      // 交互
    L"\u5173\u4e8e",      // 关于
};

// 导航项图标（用几何图形自绘，避免依赖图标字体）
enum class NavGlyph { Sliders, Palette, Cursor, Info };
const NavGlyph kNavGlyphs[4] = {NavGlyph::Sliders, NavGlyph::Palette, NavGlyph::Cursor,
                                NavGlyph::Info};

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
    for (int i = 0; i < 4; ++i) {
        rcNav_[i] = RECT{12, 104 + i * 48, kNavW - 12, 104 + i * 48 + 42};
    }

    // ---- 常规页 ----
    {
        const int c1 = 100;
        rcShadow_ = RECT{kContentX + kCardW - 60, c1 + 46, kContentX + kCardW - 24, c1 + 68};
        rcArrow_ = RECT{kContentX + kCardW - 60, c1 + 108, kContentX + kCardW - 24, c1 + 130};

        const int c2 = 300;
        rcReload_ = RECT{kContentX + kCardW - 148, c2 + 42, kContentX + kCardW - 24, c2 + 74};
        rcSysIcons_ = RECT{kContentX + kCardW - 60, c2 + 104, kContentX + kCardW - 24, c2 + 126};
    }

    // ---- 外观页 ----
    {
        const int c1 = 100;
        for (int i = 0; i < 8; ++i) {
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
    }

    // ---- 交互页 ----
    {
        const int c1 = 100;
        const int tr = kContentX + kCardW - 24;
        rcHoverExpand_ = RECT{tr - 36, c1 + 46, tr, c1 + 68};
    }

    // ---- 关于页 ----
    {
        const int c1 = 100;
        rcOpenFolder_ = RECT{kContentX + 20, c1 + 172, kContentX + 180, c1 + 204};
    }
}

int SettingsWindow::HitTest(POINT pt) const {
    for (int i = 0; i < 4; ++i) {
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
        break;
    case Page::Appearance:
        for (int i = 0; i < 8; ++i) {
            if (in(swatches_[i], 4, 4)) return ID_SWATCH0 + i;
        }
        if (in(rcRadius_, 0, 8)) return ID_RADIUS;
        if (in(rcAlpha_, 0, 8)) return ID_ALPHA;
        if (in(rcHeaderH_, 0, 8)) return ID_HEADER_H;
        if (in(rcAcrylic_, 150, 16)) return ID_ACRYLIC;
        if (in(rcShowLabels_, 150, 16)) return ID_SHOW_LABELS;
        break;
    case Page::Interaction:
        if (in(rcHoverExpand_, 150, 18)) return ID_HOVER_EXPAND;
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
    Font fTitle(&family, 22.0f, FontStyleBold, UnitPixel);
    Font fSub(&family, 12.0f, FontStyleRegular, UnitPixel);
    Font fItem(&family, 14.0f, FontStyleRegular, UnitPixel);

    StringFormat sf;
    sf.SetLineAlignment(StringAlignmentCenter);
    sf.SetFormatFlags(StringFormatFlagsNoWrap);

    SolidBrush brText(C(t.text));
    SolidBrush brSub(C(t.sub));

    g.DrawString(L"\u8bbe\u7f6e", -1, &fTitle, RectF(24.0f, 26.0f, 160.0f, 32.0f), &sf,
                 &brText);
    g.DrawString(L"meow-desk", -1, &fSub, RectF(26.0f, 60.0f, 160.0f, 18.0f), &sf, &brSub);

    for (int i = 0; i < 4; ++i) {
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
        case NavGlyph::Info: {
            GraphicsPath p;
            p.AddEllipse(cx - 6.5f, cy - 6.5f, 13.0f, 13.0f);
            g.DrawPath(&gp, &p);
            g.FillEllipse(&gb, cx - 1.5f, cy - 4.5f, 3.0f, 3.0f);
            g.FillRectangle(&gb, cx - 1.5f, cy - 0.5f, 3.0f, 5.0f);
            break;
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
        drawCard(c2, 166);
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
        for (int i = 0; i < 8; ++i) {
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
        drawCard(c2, 116);
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
        break;
    }

    case Page::Interaction: {
        g.DrawString(L"\u4ea4\u4e92", -1, &fPage, RectF((float)kContentX, 26.0f, 300.0f, 30.0f),
                     &sf, &brText);
        g.DrawString(L"\u9f20\u6807\u4e0e\u76d2\u5b50\u7684\u54cd\u5e94\u65b9\u5f0f", -1, &fSub,
                     RectF((float)kContentX + 2, 56.0f, 460.0f, 18.0f), &sf, &brSub);

        const int c1 = 100;
        drawCard(c1, 176);
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

        g.DrawString(L"\u5f00\u542f\u540e\uff0c\u60ac\u505c\u5c31\u80fd\u770b\u5230\u76d2\u5b50\u91cc\u7684\u5185\u5bb9\uff0c"
                     L"\u4e0d\u7528\u6bcf\u6b21\u90fd\u70b9\u7bad\u5934\u3002",
                     -1, &fSub,
                     RectF((float)kRowLeft, (float)c1 + 118.0f, 460.0f, 18.0f), &sf,
                     &brSub);
        g.DrawString(L"\u62d6\u52a8\u76d2\u5b50\u65f6\u4f1a\u81ea\u52a8\u907f\u8ba9\u5c4f\u5e55\u8fb9\u7f18"
                     L"\u548c\u5176\u4ed6\u76d2\u5b50\u3002",
                     -1, &fSub,
                     RectF((float)kRowLeft, (float)c1 + 140.0f, 460.0f, 18.0f), &sf,
                     &brSub);
        break;
    }

    case Page::About: {
        g.DrawString(L"\u5173\u4e8e", -1, &fPage, RectF((float)kContentX, 26.0f, 300.0f, 30.0f),
                     &sf, &brText);
        g.DrawString(L"meow-desk \u00b7 \u8f7b\u91cf\u684c\u9762\u5206\u533a\u5de5\u5177", -1,
                     &fSub, RectF((float)kContentX + 2, 56.0f, 460.0f, 18.0f), &sf, &brSub);

        const int c1 = 100;
        drawCard(c1, 220);
        g.DrawString(L"meow-desk", -1, &fSection,
                     RectF((float)kContentX + 20, (float)c1 + 16.0f, 300.0f, 24.0f), &sf,
                     &brText);

        Font fAbout(&family, 12.5f, FontStyleRegular, UnitPixel);
        const wchar_t* lines[] = {
            L"\u7248\u672c 1.0.0 \u00b7 C++ / Win32 \u539f\u751f\u5b9e\u73b0",
            L"\u684c\u9762\u56fe\u6807\u7531\u672c\u7a0b\u5e8f\u81ea\u7ed8\uff0c"
            L"\u6587\u4ef6\u59cb\u7ec8\u7559\u5728\u684c\u9762\u4e0a",
            L"\u9000\u51fa\u540e\u771f\u5b9e\u684c\u9762\u81ea\u52a8\u6062\u590d\u539f\u6837",
        };
        float ly = (float)c1 + 50.0f;
        for (const wchar_t* s : lines) {
            g.DrawString(s, -1, &fAbout, RectF((float)kContentX + 20, ly, 560.0f, 20.0f),
                         &sf, &brSub);
            ly += 22.0f;
        }

        g.DrawString(L"\u914d\u7f6e\u6587\u4ef6", -1, &fBody,
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
            g.DrawString(L"\u6253\u5f00\u914d\u7f6e\u6587\u4ef6\u5939", -1, &fBody,
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
    case ID_HOVER_EXPAND:
        g_config.autoExpandOnHover = g_config.autoExpandOnHover ? 0 : 1;
        break;
    case ID_SYSICONS:
        if (setSystemIconsShown) {
            const bool shown = getSystemIconsShown ? getSystemIconsShown() : false;
            setSystemIconsShown(!shown);
        }
        break;
    default:
        return;
    }
    NotifyChanged();
    g_config.Save();
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

    case WM_LBUTTONDOWN: {
        POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        const int id = HitTest(pt);

        if (id >= ID_NAV0 && id < ID_NAV0 + 4) {
            page_ = (Page)(id - ID_NAV0);
            hot_ = id;
            Paint();
            return 0;
        }
        if (id >= ID_SWATCH0 && id < ID_SWATCH0 + 8) {
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
            id == ID_HOVER_EXPAND || id == ID_SYSICONS) {
            Toggle(id);
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

    RECT wr{0, 0, kClientW, kClientH};
    AdjustWindowRectEx(&wr, WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU, FALSE, WS_EX_APPWINDOW);
    const int w = wr.right - wr.left;
    const int h = wr.bottom - wr.top;

    int sx = (GetSystemMetrics(SM_CXSCREEN) - w) / 2;
    int sy = (GetSystemMetrics(SM_CYSCREEN) - h) / 2;

    hwnd_ = CreateWindowExW(WS_EX_APPWINDOW, kClass, L"meow-desk \u8bbe\u7f6e",
                            WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU,
                            sx, sy, w, h, owner, nullptr, inst, this);
    if (!hwnd_) return false;

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
    ShowWindow(hwnd_, SW_SHOWNORMAL);
    SetForegroundWindow(hwnd_);
    Paint();
}

void SettingsWindow::Hide() {
    if (hwnd_) ShowWindow(hwnd_, SW_HIDE);
}

void SettingsWindow::Destroy() {
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

#include "settings_window.h"

#include <windowsx.h>
#include <dwmapi.h>
#include <objidl.h>
#include <propidl.h>
#include <gdiplus.h>

#include <algorithm>
#include <cmath>
#include <cstdio>

#include "config.h"
#include "icon_render.h"

#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "gdiplus.lib")

using namespace Gdiplus;

namespace {

const wchar_t* kClass = L"MeowDeskSettings";

const int kClientW = 560;
const int kClientH = 608;

enum : int {
    ID_NONE = -1,
    ID_SWATCH0 = 0,   // 0..7
    ID_RADIUS = 20,
    ID_ALPHA = 21,
    ID_ACRYLIC = 22,
    ID_SHADOW = 23,
    ID_ARROW = 24,
};

// Fluent 调色
const COLORREF kSwatchColors[8] = {
    RGB(250, 251, 254), RGB(232, 240, 254), RGB(224, 247, 250), RGB(232, 245, 233),
    RGB(255, 248, 225), RGB(252, 232, 230), RGB(243, 233, 245), RGB(224, 224, 224),
};

struct Theme {
    COLORREF bg;
    COLORREF card;
    COLORREF cardBorder;
    COLORREF text;
    COLORREF sub;
    COLORREF accent;
    COLORREF track;
    COLORREF devider;
};

Theme LightTheme() {
    return {RGB(243, 243, 243), RGB(255, 255, 255), RGB(229, 229, 229),
            RGB(26, 26, 26), RGB(97, 97, 97), RGB(0, 103, 192),
            RGB(224, 224, 224), RGB(236, 236, 236)};
}

Theme DarkTheme() {
    return {RGB(32, 32, 32), RGB(43, 43, 43), RGB(58, 58, 58),
            RGB(245, 245, 245), RGB(160, 160, 160), RGB(76, 194, 255),
            RGB(69, 69, 69), RGB(52, 52, 52)};
}

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
    if (d <= 0.0f) {
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

// 滑块/开关的可视轨道（比命中区域窄一点）
RECT TrackRect(const RECT& row) {
    RECT r = row;
    const int cy = (row.top + row.bottom) / 2;
    r.top = cy - 2;
    r.bottom = cy + 2;
    r.right -= 66;   // 右侧留出数值文本
    return r;
}

}  // namespace

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

void SettingsWindow::Layout() {
    const int left = 20, width = kClientW - 40;

    // 卡片 1：盒子外观
    const int c1 = 96;
    int y = c1 + 54;
    for (int i = 0; i < 8; ++i) {
        swatches_[i] = RECT{left + 100 + i * 36, y, left + 100 + i * 36 + 26, y + 26};
    }

    rcRadius_ = RECT{left + 110, c1 + 96, left + width - 24, c1 + 124};
    rcAlpha_ = RECT{left + 110, c1 + 148, left + width - 24, c1 + 176};
    rcAcrylic_ = RECT{left + width - 70, c1 + 204, left + width - 30, c1 + 224};

    // 卡片 2：桌面
    const int c2 = 400;
    rcShadow_ = RECT{left + width - 70, c2 + 56, left + width - 30, c2 + 76};
    rcArrow_ = RECT{left + width - 70, c2 + 106, left + width - 30, c2 + 126};
}

int SettingsWindow::HitTest(POINT pt) const {
    for (int i = 0; i < 8; ++i) {
        const RECT& r = swatches_[i];
        if (pt.x >= r.left - 3 && pt.x < r.right + 3 && pt.y >= r.top - 3 && pt.y < r.bottom + 3)
            return ID_SWATCH0 + i;
    }
    auto inside = [&](const RECT& r, int padY) {
        return pt.x >= r.left && pt.x < r.right && pt.y >= r.top - padY &&
               pt.y < r.bottom + padY;
    };
    if (inside(rcRadius_, 6)) return ID_RADIUS;
    if (inside(rcAlpha_, 6)) return ID_ALPHA;

    auto nearToggle = [&](const RECT& r) {
        return pt.x >= r.left - 120 && pt.x < r.right + 12 && pt.y >= r.top - 10 &&
               pt.y < r.bottom + 10;
    };
    if (nearToggle(rcAcrylic_)) return ID_ACRYLIC;
    if (nearToggle(rcShadow_)) return ID_SHADOW;
    if (nearToggle(rcArrow_)) return ID_ARROW;
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

void SettingsWindow::Paint() {
    if (!hwnd_) return;
    RECT cr{};
    GetClientRect(hwnd_, &cr);
    const int w = cr.right, h = cr.bottom;
    if (!EnsureBackbuffer(w, h)) return;

    const Theme t = dark_ ? DarkTheme() : LightTheme();
    const COLORREF base = g_config.fences.empty()
                              ? RGB(250, 251, 254)
                              : (COLORREF)g_config.fences[0].color;
    const int radius = g_config.fences.empty() ? 12 : g_config.fences[0].radius;
    const int alpha = g_config.fences.empty() ? 205 : g_config.fences[0].alpha;
    const bool acrylic = !g_config.fences.empty() && g_config.fences[0].acrylic != 0;

    Bitmap target(w, h, w * 4, PixelFormat32bppPARGB, reinterpret_cast<BYTE*>(bits_));
    Graphics g(&target);
    g.SetSmoothingMode(SmoothingModeAntiAlias);
    g.SetPixelOffsetMode(PixelOffsetModeHighQuality);
    g.SetTextRenderingHint(TextRenderingHintAntiAlias);
    g.Clear(C(t.bg));

    FontFamily family(L"Microsoft YaHei UI");
    Font fTitle(&family, 23.0f, FontStyleBold, UnitPixel);
    Font fSub(&family, 12.5f, FontStyleRegular, UnitPixel);
    Font fSection(&family, 15.0f, FontStyleBold, UnitPixel);
    Font fBody(&family, 13.5f, FontStyleRegular, UnitPixel);
    Font fValue(&family, 13.0f, FontStyleRegular, UnitPixel);

    StringFormat sfLeft;
    sfLeft.SetLineAlignment(StringAlignmentCenter);
    sfLeft.SetFormatFlags(StringFormatFlagsNoWrap);

    SolidBrush brText(C(t.text));
    SolidBrush brSub(C(t.sub));

    // ---- 标题 ----
    g.DrawString(L"\u8bbe\u7f6e", -1, &fTitle, RectF(24.0f, 22.0f, 300.0f, 34.0f),
                 &sfLeft, &brText);
    g.DrawString(L"meow-desk \u00b7 \u684c\u9762\u5206\u533a\u5de5\u5177", -1, &fSub,
                 RectF(26.0f, 56.0f, 400.0f, 20.0f), &sfLeft, &brSub);

    auto drawCard = [&](int y, int ch) {
        GraphicsPath p;
        AddRound(p, RectF(20.0f, (float)y, (float)(kClientW - 40), (float)ch), 10.0f);
        SolidBrush fill(C(t.card));
        g.FillPath(&fill, &p);
        Pen pen(C(t.cardBorder), 1.0f);
        g.DrawPath(&pen, &p);
    };

    // ---- 卡片 1：盒子外观 ----
    {
        const int c1 = 96;
        drawCard(c1, 276);
        g.DrawString(L"\u76d2\u5b50\u5916\u89c2", -1, &fSection,
                     RectF(40.0f, (float)c1 + 16.0f, 260.0f, 24.0f), &sfLeft, &brText);

        // 颜色
        g.DrawString(L"\u989c\u8272", -1, &fBody,
                     RectF(40.0f, (float)(c1 + 54), 70.0f, 26.0f), &sfLeft, &brText);
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

        // 圆角
        g.DrawString(L"\u5706\u89d2", -1, &fBody,
                     RectF(40.0f, (float)rcRadius_.top, 90.0f, 28.0f), &sfLeft, &brText);
        // 透明度
        g.DrawString(L"\u900f\u660e\u5ea6", -1, &fBody,
                     RectF(40.0f, (float)rcAlpha_.top, 90.0f, 28.0f), &sfLeft, &brText);

        auto drawSlider = [&](const RECT& row, int lo, int hi, int val) {
            RECT tr = TrackRect(row);
            GraphicsPath bgp;
            AddRound(bgp, RectF((float)tr.left, (float)tr.top,
                                (float)(tr.right - tr.left), 4.0f), 2.0f);
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
            SolidBrush knob(C(dark_ ? RGB(255, 255, 255) : RGB(255, 255, 255)));
            g.FillEllipse(&knob, cx - 9.0f, cy - 9.0f, 18.0f, 18.0f);
            Pen kp(C(dark_ ? RGB(70, 70, 70) : RGB(180, 180, 180)), 1.0f);
            g.DrawEllipse(&kp, cx - 9.0f, cy - 9.0f, 18.0f, 18.0f);
            SolidBrush dot(C(t.accent));
            g.FillEllipse(&dot, cx - 4.0f, cy - 4.0f, 8.0f, 8.0f);
        };

        drawSlider(rcRadius_, 0, 28, radius);
        {
            wchar_t buf[16];
            swprintf_s(buf, L"%d", radius);
            StringFormat right;
            right.SetAlignment(StringAlignmentFar);
            right.SetLineAlignment(StringAlignmentCenter);
            g.DrawString(buf, -1, &fValue,
                         RectF((float)(rcRadius_.right - 56), (float)rcRadius_.top, 56.0f, 28.0f),
                         &right, &brSub);
        }

        drawSlider(rcAlpha_, 40, 255, alpha);
        {
            wchar_t buf[16];
            swprintf_s(buf, L"%d%%", (int)std::lround(alpha * 100.0 / 255.0));
            StringFormat right;
            right.SetAlignment(StringAlignmentFar);
            right.SetLineAlignment(StringAlignmentCenter);
            g.DrawString(buf, -1, &fValue,
                         RectF((float)(rcAlpha_.right - 56), (float)rcAlpha_.top, 56.0f, 28.0f),
                         &right, &brSub);
        }

        // 亚克力
        g.DrawString(L"Win11 \u4e9a\u514b\u529b\u6548\u679c", -1, &fBody,
                     RectF(40.0f, (float)(rcAcrylic_.top - 8), 260.0f, 28.0f), &sfLeft, &brText);
        g.DrawString(L"\u6a21\u7cca\u80cc\u666f\uff0c\u66f4\u63a5\u8fd1\u7cfb\u7edf\u539f\u751f\u8d28\u611f",
                     -1, &fSub, RectF(40.0f, (float)(rcAcrylic_.top + 12), 300.0f, 18.0f),
                     &sfLeft, &brSub);
    }

    // ---- 卡片 2：桌面 ----
    {
        const int c2 = 400;
        drawCard(c2, 152);
        g.DrawString(L"\u684c\u9762", -1, &fSection,
                     RectF(40.0f, (float)c2 + 14.0f, 200.0f, 24.0f), &sfLeft, &brText);

        g.DrawString(L"\u56fe\u6807\u6587\u5b57\u9634\u5f71", -1, &fBody,
                     RectF(40.0f, (float)(rcShadow_.top - 8), 260.0f, 28.0f), &sfLeft, &brText);
        g.DrawString(L"\u767d\u5b57\u5916\u4e00\u5708\u67d4\u548c\u6697\u8fb9\uff0c\u4eae\u58c1\u7eb8\u4e0a\u4e5f\u770b\u5f97\u6e05",
                     -1, &fSub, RectF(40.0f, (float)(rcShadow_.top + 12), 340.0f, 18.0f),
                     &sfLeft, &brSub);

        g.DrawString(L"\u5feb\u6377\u65b9\u5f0f\u7bad\u5934", -1, &fBody,
                     RectF(40.0f, (float)(rcArrow_.top - 8), 260.0f, 28.0f), &sfLeft, &brText);
        g.DrawString(L"\u56fe\u6807\u5de6\u4e0b\u89d2\u663e\u793a\u5feb\u6377\u65b9\u5f0f\u6807\u8bb0",
                     -1, &fSub, RectF(40.0f, (float)(rcArrow_.top + 12), 340.0f, 18.0f),
                     &sfLeft, &brSub);
    }

    // ---- 开关 ----
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
    drawToggle(rcAcrylic_, acrylic);
    drawToggle(rcShadow_, g_config.textShadow != 0);
    drawToggle(rcArrow_, g_config.shortcutArrow != 0);

    // ---- 页脚 ----
    StringFormat center;
    center.SetAlignment(StringAlignmentCenter);
    g.DrawString(L"\u8bbe\u7f6e\u4f1a\u7acb\u5373\u751f\u6548\uff0c\u5e76\u4fdd\u5b58\u5230 %APPDATA%\\meow-desk\\config.ini",
                 -1, &fSub, RectF(20.0f, (float)h - 28.0f, (float)kClientW - 40.0f, 18.0f),
                 &center, &brSub);

    g.Flush(FlushIntentionSync);

    HDC dc = GetDC(hwnd_);
    BitBlt(dc, 0, 0, w, h, memDC_, 0, 0, SRCCOPY);
    ReleaseDC(hwnd_, dc);
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
        if (id >= ID_SWATCH0 && id < ID_SWATCH0 + 8) {
            const COLORREF c = kSwatchColors[id - ID_SWATCH0];
            for (auto& f : g_config.fences) f.color = (int)c;
            NotifyChanged();
            g_config.Save();
            Paint();
            return 0;
        }
        if (id == ID_ACRYLIC) {
            const int nv = g_config.fences.empty() ? 1 : (g_config.fences[0].acrylic ? 0 : 1);
            for (auto& f : g_config.fences) f.acrylic = nv;
            NotifyChanged();
            g_config.Save();
            Paint();
            return 0;
        }
        if (id == ID_SHADOW) {
            g_config.textShadow = g_config.textShadow ? 0 : 1;
            NotifyChanged();
            g_config.Save();
            Paint();
            return 0;
        }
        if (id == ID_ARROW) {
            g_config.shortcutArrow = g_config.shortcutArrow ? 0 : 1;
            NotifyChanged();
            g_config.Save();
            Paint();
            return 0;
        }
        if (id == ID_RADIUS || id == ID_ALPHA) {
            sliderDrag_ = id;
            SetCapture(h);
            // 立刻按点击位置设值
            SendMessageW(h, WM_MOUSEMOVE, 0, lp);
            return 0;
        }
        return 0;
    }

    case WM_MOUSEMOVE: {
        POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        if (sliderDrag_ >= 0) {
            const RECT& row = (sliderDrag_ == ID_RADIUS) ? rcRadius_ : rcAlpha_;
            RECT tr = TrackRect(row);
            const int lo = (sliderDrag_ == ID_RADIUS) ? 0 : 40;
            const int hi = (sliderDrag_ == ID_RADIUS) ? 28 : 255;
            const int span = tr.right - tr.left;
            int v = lo;
            if (span > 0) {
                double t01 = (double)(pt.x - tr.left) / (double)span;
                t01 = std::clamp(t01, 0.0, 1.0);
                v = lo + (int)std::lround(t01 * (hi - lo));
            }
            for (auto& f : g_config.fences) {
                if (sliderDrag_ == ID_RADIUS) f.radius = v;
                else f.alpha = v;
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
            if (id >= ID_SWATCH0 || id == ID_ACRYLIC || id == ID_SHADOW || id == ID_ARROW) {
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
    AdjustWindowRectEx(&wr, WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU, FALSE,
                       WS_EX_APPWINDOW);
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

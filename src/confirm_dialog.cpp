#include "confirm_dialog.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

#include <windowsx.h>

// GDI+ 的头文件顺序：Image / PropertyItem 需要 IStream / PROPID
#include <objidl.h>
#include <propidl.h>
#include <gdiplus.h>

#include "icon_render.h"

using namespace Gdiplus;

namespace {

const wchar_t* kClass = L"MeowDeskConfirm";

const int kShadow = 12;     // 阴影留白（客户区比卡片每边大这么多）
const int kRound = 10;      // 卡片圆角
const int kCardW = 468;     // 卡片固定宽度
const int kPadX = 26;       // 卡片左右内边距
const int kBtnW = 96;       // 按钮宽
const int kBtnH = 34;       // 按钮高
const int kBtnGap = 10;     // 两个按钮之间
const int kLineH = 21;      // 正文行高

bool SystemUsesLightTheme() {
    HKEY k = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER,
                      L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize", 0,
                      KEY_READ, &k) != ERROR_SUCCESS)
        return true;
    DWORD v = 1, sz = sizeof(v), type = 0;
    RegQueryValueExW(k, L"AppsUseLightTheme", nullptr, &type, reinterpret_cast<LPBYTE>(&v), &sz);
    RegCloseKey(k);
    return v != 0;
}

void AddRoundRect(GraphicsPath& p, const RectF& r, float rad) {
    if (rad <= 0.0f) {
        p.AddRectangle(r);
        return;
    }
    const float d = rad * 2.0f;
    if (d > r.Width) rad = r.Width * 0.5f;
    if (d > r.Height) rad = r.Height * 0.5f;
    p.AddArc(r.X, r.Y, rad * 2, rad * 2, 180.0f, 90.0f);
    p.AddArc(r.X + r.Width - rad * 2, r.Y, rad * 2, rad * 2, 270.0f, 90.0f);
    p.AddArc(r.X + r.Width - rad * 2, r.Y + r.Height - rad * 2, rad * 2, rad * 2, 0.0f, 90.0f);
    p.AddArc(r.X, r.Y + r.Height - rad * 2, rad * 2, rad * 2, 90.0f, 90.0f);
    p.CloseFigure();
}

// 正文按 \n 拆行，最多 3 行（再多会压到按钮上）
std::vector<std::wstring> SplitLines(const std::wstring& s) {
    std::vector<std::wstring> out;
    size_t start = 0;
    while (start <= s.size() && out.size() < 3) {
        const size_t nl = s.find(L'\n', start);
        if (nl == std::wstring::npos) {
            out.push_back(s.substr(start));
            break;
        }
        out.push_back(s.substr(start, nl - start));
        start = nl + 1;
    }
    if (out.empty()) out.push_back(std::wstring());
    return out;
}

}  // namespace

bool ConfirmBox::EnsureWindow() {
    if (hwnd_) return true;
    EnsureGdiplus();

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = WndProcStatic;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = kClass;
    RegisterClassExW(&wc);   // 已注册会失败，忽略

    // 要能拿键盘（Enter / Esc），所以不带 WS_EX_NOACTIVATE
    hwnd_ = CreateWindowExW(WS_EX_LAYERED | WS_EX_TOOLWINDOW | WS_EX_TOPMOST, kClass, L"",
                            WS_POPUP, 0, 0, 10, 10, nullptr, nullptr,
                            GetModuleHandleW(nullptr), this);
    return hwnd_ != nullptr;
}

void ConfirmBox::EnsureBuffer(int w, int h) {
    if (w <= 0 || h <= 0) return;
    if (memDC_ && bufW_ == w && bufH_ == h) return;
    if (memDC_) { DeleteDC(memDC_); memDC_ = nullptr; }
    if (dib_) { DeleteObject(dib_); dib_ = nullptr; }
    bits_ = nullptr;
    if (CreateDib(w, h, memDC_, dib_, bits_)) {
        bufW_ = w;
        bufH_ = h;
    } else {
        bufW_ = bufH_ = 0;
    }
}

void ConfirmBox::Relayout() {
    const std::vector<std::wstring> lines = SplitLines(text_);
    const int linesH = (int)lines.size() * kLineH;

    cardW_ = kCardW;
    // 上 24 + 标题 26 + 段距 10 + 正文 + 段距 22 + 按钮 34 + 下 24
    cardH_ = 24 + 26 + 10 + linesH + 22 + kBtnH + 24;

    EnsureBuffer(cardW_ + kShadow * 2, cardH_ + kShadow * 2);

    const int cardX = kShadow;
    const int cardY = kShadow;
    const int btnTop = cardY + cardH_ - 24 - kBtnH;
    const int okRight = cardX + cardW_ - kPadX;
    rcOk_ = RECT{okRight - kBtnW, btnTop, okRight, btnTop + kBtnH};
    rcCancel_ = RECT{rcOk_.left - kBtnGap - kBtnW, btnTop, rcOk_.left - kBtnGap,
                     btnTop + kBtnH};
}

void ConfirmBox::Paint() {
    if (!memDC_ || !bits_ || bufW_ <= 0 || bufH_ <= 0) return;
    std::memset(bits_, 0, (size_t)bufW_ * (size_t)bufH_ * 4);

    const bool light = SystemUsesLightTheme();
    const ARGB bg = light ? 0xFFFBFBFD : 0xFF2C2C30;
    const ARGB border = light ? 0xFFD9D9DE : 0xFF4A4A52;
    const ARGB fg = light ? 0xFF1B1B1F : 0xFFF0F0F4;
    const ARGB fgDim = light ? 0xFF6E6E76 : 0xFFA8A8B0;
    // 危险动作的按钮：Windows 的「删除红」，浅深色都用同一个，够醒目
    const ARGB danger = light ? 0xFFC42B1C : 0xFFD93A2B;
    const ARGB dangerHot = light ? 0xFFD13A2A : 0xFFE64C3C;

    {
        Bitmap target(bufW_, bufH_, bufW_ * 4, PixelFormat32bppPARGB,
                      reinterpret_cast<BYTE*>(bits_));
        Graphics g(&target);
        g.SetSmoothingMode(SmoothingModeAntiAlias);
        g.SetPixelOffsetMode(PixelOffsetModeHighQuality);
        // 分层窗口 + alpha：不能用 ClearType，会出彩色杂边
        g.SetTextRenderingHint(TextRenderingHintAntiAlias);

        const RectF body((float)kShadow, (float)kShadow, (float)cardW_, (float)cardH_);

        // 阴影：由外到内叠几层，越外越淡（和弹出菜单同一个配方）
        for (int i = kShadow; i >= 1; --i) {
            const int a = std::max(2, 7 - i / 2);
            GraphicsPath sp;
            AddRoundRect(sp,
                         RectF(body.X - i * 0.45f, body.Y - i * 0.35f + 2.0f,
                               body.Width + i * 0.9f, body.Height + i * 0.8f),
                         kRound + i * 0.5f);
            SolidBrush sb(Color((BYTE)a, 0, 0, 0));
            g.FillPath(&sb, &sp);
        }

        GraphicsPath card;
        AddRoundRect(card, body, (float)kRound);
        SolidBrush bgBrush(bg);
        g.FillPath(&bgBrush, &card);
        Pen pen(border, 1.0f);
        g.DrawPath(&pen, &card);

        FontFamily fam(L"Microsoft YaHei UI");
        Font fTitle(&fam, 15.0f, FontStyleBold, UnitPixel);
        Font fBody(&fam, 13.0f, FontStyleRegular, UnitPixel);
        Font fBtn(&fam, 13.0f, FontStyleRegular, UnitPixel);

        StringFormat sf;
        sf.SetLineAlignment(StringAlignmentCenter);
        sf.SetFormatFlags(StringFormatFlagsNoWrap);
        sf.SetTrimming(StringTrimmingEllipsisCharacter);

        // 标题
        {
            SolidBrush tb(fg);
            g.DrawString(title_.c_str(), -1, &fTitle,
                         RectF((float)kShadow + kPadX, (float)kShadow + 22.0f,
                               (float)cardW_ - kPadX * 2, 26.0f),
                         &sf, &tb);
        }

        // 正文（逐行画，行高固定，行数在 Relayout 里已经进过尺寸）
        {
            SolidBrush tb(fgDim);
            const std::vector<std::wstring> lines = SplitLines(text_);
            float y = (float)kShadow + 24.0f + 26.0f + 10.0f;
            for (const auto& ln : lines) {
                g.DrawString(ln.c_str(), -1, &fBody,
                             RectF((float)kShadow + kPadX, y,
                                   (float)cardW_ - kPadX * 2, (float)kLineH),
                             &sf, &tb);
                y += kLineH;
            }
        }

        // 按钮：取消（描边款）+ 确定（危险红实心）
        auto drawButton = [&](const RECT& r, const std::wstring& label, bool dangerStyle,
                              bool hot) {
            const RectF rf((float)r.left, (float)r.top, (float)(r.right - r.left),
                           (float)(r.bottom - r.top));
            GraphicsPath bp;
            AddRoundRect(bp, rf, 7.0f);
            if (dangerStyle) {
                SolidBrush fb(hot ? dangerHot : danger);
                g.FillPath(&fb, &bp);
                SolidBrush tb{Color(255, 255, 255, 255)};
                StringFormat cf;
                cf.SetAlignment(StringAlignmentCenter);
                cf.SetLineAlignment(StringAlignmentCenter);
                cf.SetFormatFlags(StringFormatFlagsNoWrap);
                g.DrawString(label.c_str(), -1, &fBtn, rf, &cf, &tb);
            } else {
                if (hot) {
                    SolidBrush hb(light ? Color(20, 0, 0, 0) : Color(26, 255, 255, 255));
                    g.FillPath(&hb, &bp);
                }
                Pen ep(light ? Color(255, 208, 208, 214) : Color(255, 84, 84, 92), 1.0f);
                g.DrawPath(&ep, &bp);
                SolidBrush tb{Color(fg)};
                StringFormat cf;
                cf.SetAlignment(StringAlignmentCenter);
                cf.SetLineAlignment(StringAlignmentCenter);
                cf.SetFormatFlags(StringFormatFlagsNoWrap);
                g.DrawString(label.c_str(), -1, &fBtn, rf, &cf, &tb);
            }
        };
        drawButton(rcCancel_, L"\u53d6\u6d88", false, hover_ == 0);          // 取消
        drawButton(rcOk_, okText_, true, hover_ == 1);

        g.Flush(FlushIntentionSync);
    }
}

void ConfirmBox::Present() {
    if (!hwnd_ || !memDC_) return;
    BLENDFUNCTION bf{};
    bf.BlendOp = AC_SRC_OVER;
    bf.SourceConstantAlpha = 255;
    bf.AlphaFormat = AC_SRC_ALPHA;

    RECT wr{};
    GetWindowRect(hwnd_, &wr);
    HDC screen = GetDC(nullptr);
    POINT dst{wr.left, wr.top};
    SIZE size{bufW_, bufH_};
    POINT src{0, 0};
    UpdateLayeredWindow(hwnd_, screen, &dst, &size, memDC_, &src, 0, &bf, ULW_ALPHA);
    ReleaseDC(nullptr, screen);
}

int ConfirmBox::HitTest(POINT c) const {
    auto in = [&](const RECT& r) {
        return c.x >= r.left && c.x < r.right && c.y >= r.top && c.y < r.bottom;
    };
    if (in(rcOk_)) return 1;
    if (in(rcCancel_)) return 0;
    return -1;
}

void ConfirmBox::Accept() {
    // Hide() 会清掉回调，先拷一份
    auto cb = onOk_;
    Hide();
    if (cb) cb();
}

void ConfirmBox::Show(HWND owner, const std::wstring& title, const std::wstring& text,
                      const std::wstring& okText, std::function<void()> onOk) {
    if (!EnsureWindow()) return;
    owner_ = owner;
    title_ = title;
    text_ = text;
    okText_ = okText.empty() ? L"\u786e\u5b9a" : okText;   // 确定
    onOk_ = std::move(onOk);
    activated_ = false;
    hover_ = -1;
    press_ = -1;

    Relayout();
    if (bufW_ <= 0) return;

    // 摆位：居中于 owner；owner 太靠边就夹进工作区
    RECT anchor{};
    if (!owner_ || !GetWindowRect(owner_, &anchor)) {
        anchor = RECT{0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN)};
    }
    int x = (anchor.left + anchor.right - bufW_) / 2;
    int y = (anchor.top + anchor.bottom - bufH_) / 2;

    MONITORINFO mi{};
    mi.cbSize = sizeof(mi);
    HMONITOR mon = MonitorFromPoint(POINT{(anchor.left + anchor.right) / 2, anchor.top},
                                    MONITOR_DEFAULTTONEAREST);
    RECT lim{0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN)};
    if (mon && GetMonitorInfoW(mon, &mi)) lim = mi.rcWork;
    if (x < lim.left) x = lim.left;
    if (y < lim.top) y = lim.top;
    if (x + bufW_ > lim.right) x = lim.right - bufW_;
    if (y + bufH_ > lim.bottom) y = lim.bottom - bufH_;

    SetWindowPos(hwnd_, HWND_TOPMOST, x, y, bufW_, bufH_, SWP_SHOWWINDOW | SWP_NOACTIVATE);
    Paint();
    Present();
    // 要能按 Enter / Esc，所以必须真的激活（盒子是 NOACTIVATE 的，它不会抢）
    ShowWindow(hwnd_, SW_SHOWNORMAL);
    SetForegroundWindow(hwnd_);
    SetActiveWindow(hwnd_);
    SetFocus(hwnd_);
}

void ConfirmBox::Hide() {
    onOk_ = nullptr;
    activated_ = false;
    hover_ = -1;
    press_ = -1;
    if (hwnd_ && IsWindowVisible(hwnd_)) ShowWindow(hwnd_, SW_HIDE);
}

LRESULT CALLBACK ConfirmBox::WndProcStatic(HWND h, UINT m, WPARAM w, LPARAM l) {
    ConfirmBox* self = reinterpret_cast<ConfirmBox*>(GetWindowLongPtrW(h, GWLP_USERDATA));
    if (m == WM_NCCREATE) {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(l);
        self = reinterpret_cast<ConfirmBox*>(cs->lpCreateParams);
        SetWindowLongPtrW(h, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    }
    if (self) return self->WndProc(h, m, w, l);
    return DefWindowProcW(h, m, w, l);
}

LRESULT ConfirmBox::WndProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_MOUSEACTIVATE:
        return MA_ACTIVATE;

    case WM_ACTIVATE:
        if (LOWORD(wp) == WA_INACTIVE) {
            // 点到别处 = 放弃（和系统对话框的「点外面取消」一致）
            if (activated_) Hide();
        } else {
            activated_ = true;
        }
        return 0;

    case WM_KEYDOWN:
        if (wp == VK_RETURN) {
            Accept();
            return 0;
        }
        if (wp == VK_ESCAPE) {
            Hide();
            return 0;
        }
        break;

    case WM_MOUSEMOVE: {
        POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        const int hot = HitTest(pt);
        if (hot != hover_) {
            hover_ = hot;
            Paint();
            Present();
        }
        TRACKMOUSEEVENT tme{sizeof(tme), TME_LEAVE, h, 0};
        TrackMouseEvent(&tme);
        return 0;
    }

    case WM_MOUSELEAVE:
        if (hover_ != -1) {
            hover_ = -1;
            Paint();
            Present();
        }
        return 0;

    case WM_SETCURSOR: {
        if (LOWORD(lp) == HTCLIENT) {
            POINT pt{};
            GetCursorPos(&pt);
            ScreenToClient(h, &pt);
            if (HitTest(pt) >= 0) {
                SetCursor(LoadCursorW(nullptr, IDC_HAND));
                return TRUE;
            }
        }
        break;
    }

    case WM_LBUTTONDOWN: {
        POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        press_ = HitTest(pt);
        if (press_ >= 0) SetCapture(h);
        return 0;
    }

    case WM_LBUTTONUP: {
        POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        if (GetCapture() == h) ReleaseCapture();
        const int was = press_;
        press_ = -1;
        const int now = HitTest(pt);
        if (was >= 0 && now == was) {
            if (was == 1) Accept();
            else Hide();
        }
        return 0;
    }

    case WM_CLOSE:
        Hide();
        return 0;

    default:
        break;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

#include "popup_menu.h"

#include "icon_render.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <cwchar>

#include <windowsx.h>

// gdiplus.h 里的 Image / PropertyItem 用到 IStream、PROPID，
// 不先带上 objidl.h / propidl.h 会一片 "未定义标识符"
#include <objidl.h>
#include <propidl.h>
#include <gdiplus.h>

using namespace Gdiplus;

namespace {

const wchar_t* kMenuClass = L"MeowDeskPopupMenu";

const int kItemH = 32;      // 单行高
const int kSepH = 9;        // 分隔线占位
const int kPadL = 30;       // 左侧：勾选标记
const int kPadR = 34;       // 右侧：子菜单箭头
const int kPadV = 5;        // 上下边距
const int kShadow = 10;     // 阴影留白
const int kRound = 8;       // 圆角
// 阴影单圈 alpha 的基准：第 i 圈取 (kShadowAlpha - i)，越靠外越淡。
// 注意这些圈是**叠加**的（同一像素会被多圈覆盖），所以单圈 alpha 必须压得很低：
// 原来用 16-i（6..15），叠到卡片边缘接近 46% 的黑，看起来像一圈灰边；
// 改成 11-i（3..10）叠完约 20%，柔和很多。
const int kShadowAlpha = 11;
// 定时检查「鼠标是不是已经跑到菜单外面并按下了键」，用来收菜单。
// 只依赖 WM_ACTIVATE 不够：菜单窗口有时拿不到前台（没有真实用户输入时
// SetForegroundWindow 会被系统拒绝），那就永远等不到失活。
const UINT_PTR kWatchTimer = 7;

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

}  // namespace

bool PopupMenu::Create() {
    if (hwnd_) return true;

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW | CS_DBLCLKS;
    wc.lpfnWndProc = WndProcStatic;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = kMenuClass;
    RegisterClassExW(&wc);   // 已注册会失败，忽略

    // WS_EX_LAYERED 是 UpdateLayeredWindow 的前提，漏了它窗口会是全透明的一片
    hwnd_ = CreateWindowExW(WS_EX_LAYERED | WS_EX_TOOLWINDOW | WS_EX_TOPMOST, kMenuClass, L"",
                            WS_POPUP, 0, 0, 10, 10, nullptr, nullptr,
                            GetModuleHandleW(nullptr), this);
    return hwnd_ != nullptr;
}

void PopupMenu::Release() {
    CloseSub();
    if (hwnd_) {
        DestroyWindow(hwnd_);
        hwnd_ = nullptr;
    }
}

void PopupMenu::EnsureBuffer(int w, int h) {
    if (w <= 0 || h <= 0) return;
    if (memDC_ && bufW_ == w && bufH_ == h) return;
    if (memDC_) { DeleteDC(memDC_); memDC_ = nullptr; }
    if (dib_) { DeleteObject(dib_); dib_ = nullptr; }
    bits_ = nullptr;
    if (!CreateDib(w, h, memDC_, dib_, bits_)) {
        bufW_ = bufH_ = 0;
    } else {
        bufW_ = w;
        bufH_ = h;
    }
}

void PopupMenu::Layout() {
    HDC screen = GetDC(nullptr);
    Graphics probe(screen);
    FontFamily fam(L"Microsoft YaHei UI");
    Font font(&fam, 13.0f, FontStyleRegular, UnitPixel);

    int maxText = 40;
    for (const auto& it : items_) {
        if (it.separator) continue;
        RectF box;
        probe.MeasureString(it.text.c_str(), -1, &font, PointF(0, 0), &box);
        maxText = std::max(maxText, (int)std::ceil(box.Width));
    }
    ReleaseDC(nullptr, screen);

    contentW_ = kPadL + maxText + kPadR;
    int h = kPadV * 2;
    for (const auto& it : items_) h += it.separator ? kSepH : kItemH;
    contentH_ = h;

    w_ = contentW_ + kShadow * 2;
    h_ = contentH_ + kShadow * 2;

    // 每项的客户区矩形（含阴影偏移）
    itemRects_.clear();
    itemRects_.reserve(items_.size());
    int y = kShadow + kPadV;
    for (const auto& it : items_) {
        const int ih = it.separator ? kSepH : kItemH;
        RECT r{kShadow, y, kShadow + contentW_, y + ih};
        itemRects_.push_back(r);
        y += ih;
    }
}

void PopupMenu::Paint() {
    if (!memDC_ || !bits_) return;
    std::memset(bits_, 0, (size_t)bufW_ * bufH_ * 4);

    light_ = SystemUsesLightTheme();
    const ARGB bg = light_ ? 0xFFFBFBFD : 0xFF2C2C30;
    const ARGB border = light_ ? 0xFFD9D9DE : 0xFF4A4A52;
    const ARGB fg = light_ ? 0xFF1B1B1F : 0xFFF0F0F4;
    const ARGB fgDim = light_ ? 0xFF8A8A90 : 0xFF909098;
    const ARGB hot = light_ ? 0x1E0067C0 : 0x2E4CC2FF;
    const ARGB accent = light_ ? 0xFF0067C0 : 0xFF60CDFF;

    {
        Bitmap target(bufW_, bufH_, bufW_ * 4, PixelFormat32bppPARGB,
                      reinterpret_cast<BYTE*>(bits_));
        Graphics g(&target);
        g.SetSmoothingMode(SmoothingModeAntiAlias);
        g.SetPixelOffsetMode(PixelOffsetModeHighQuality);
        // 分层窗口 + alpha，不能用 ClearType（会出彩色杂边）
        g.SetTextRenderingHint(TextRenderingHintAntiAlias);

        const RectF body((float)kShadow, (float)kShadow, (float)contentW_, (float)contentH_);

        // 阴影：由外到内叠几层，越外越淡
        for (int i = kShadow; i >= 1; --i) {
            const int a = std::max(3, kShadowAlpha - i);
            GraphicsPath sp;
            AddRoundRect(sp, RectF(body.X - i * 0.45f, body.Y - i * 0.35f + 2.0f,
                                   body.Width + i * 0.9f, body.Height + i * 0.8f),
                         kRound + i * 0.5f);
            SolidBrush sb(Color((BYTE)a, 0, 0, 0));
            g.FillPath(&sb, &sp);
        }

        // 卡片
        GraphicsPath card;
        AddRoundRect(card, body, (float)kRound);
        SolidBrush bgBrush(bg);
        g.FillPath(&bgBrush, &card);
        Pen pen(border, 1.0f);
        g.DrawPath(&pen, &card);

        FontFamily fam(L"Microsoft YaHei UI");
        Font font(&fam, 13.0f, FontStyleRegular, UnitPixel);
        StringFormat sf;
        sf.SetLineAlignment(StringAlignmentCenter);
        sf.SetTrimming(StringTrimmingEllipsisCharacter);
        sf.SetFormatFlags(StringFormatFlagsNoWrap);

        for (size_t i = 0; i < items_.size(); ++i) {
            const PopupMenuItem& it = items_[i];
            const RECT& r = itemRects_[i];

            if (it.separator) {
                // 用填充矩形画线，不用 DrawLine：抗锯齿会把 1px 线摊成两行半透明，
                // 在浅色卡片上几乎看不见
                const float ly = std::floor((float)(r.top + r.bottom) * 0.5f);
                SolidBrush sb(light_ ? Color(255, 202, 202, 212) : Color(255, 78, 78, 88));
                g.FillRectangle(&sb, RectF((float)r.left + 10.0f, ly,
                                           (float)(r.right - r.left) - 20.0f, 1.0f));
                continue;
            }

            const bool hov = ((int)i == hover_) && !it.disabled;
            if (hov) {
                GraphicsPath hp;
                AddRoundRect(hp, RectF((float)r.left + 4.0f, (float)r.top + 1.0f,
                                       (float)(r.right - r.left) - 8.0f, (float)kItemH - 2.0f),
                             5.0f);
                SolidBrush hb{Color(hot)};
                g.FillPath(&hb, &hp);
            }

            const float cy = (float)(r.top + r.bottom) * 0.5f;
            const ARGB textColor = it.disabled ? fgDim : fg;

            // 勾选标记：对勾
            if (it.checked && !it.separator) {
                Pen cp(accent, 1.9f);
                cp.SetStartCap(LineCapRound);
                cp.SetEndCap(LineCapRound);
                const float cx = (float)r.left + 15.0f;
                g.DrawLine(&cp, cx - 4.0f, cy, cx - 1.0f, cy + 3.5f);
                g.DrawLine(&cp, cx - 1.0f, cy + 3.5f, cx + 5.0f, cy - 4.0f);
            }

            RectF textRect((float)r.left + kPadL - 4.0f, (float)r.top,
                           (float)(r.right - r.left - kPadL - kPadR + 4), (float)kItemH);
            SolidBrush tb(textColor);
            g.DrawString(it.text.c_str(), -1, &font, textRect, &sf, &tb);

            // 子菜单箭头 ›
            if (it.hasSub()) {
                Pen ap(Color(it.disabled ? fgDim : (light_ ? 0xFF5A5A60 : 0xFFB8B8C0)), 1.5f);
                ap.SetStartCap(LineCapRound);
                ap.SetEndCap(LineCapRound);
                const float ax = (float)r.right - 16.0f;
                g.DrawLine(&ap, ax - 2.5f, cy - 4.5f, ax + 2.0f, cy);
                g.DrawLine(&ap, ax + 2.0f, cy, ax - 2.5f, cy + 4.5f);
            }
        }

        g.Flush(FlushIntentionSync);
    }
}

void PopupMenu::Present() {
    if (!hwnd_ || !memDC_) return;
    BLENDFUNCTION bf{};
    bf.BlendOp = AC_SRC_OVER;
    bf.SourceConstantAlpha = 255;
    bf.AlphaFormat = AC_SRC_ALPHA;

    HDC screen = GetDC(nullptr);
    POINT dst{0, 0};
    RECT wr{};
    GetWindowRect(hwnd_, &wr);
    dst.x = wr.left;
    dst.y = wr.top;
    SIZE size{bufW_, bufH_};
    POINT src{0, 0};
    UpdateLayeredWindow(hwnd_, screen, &dst, &size, memDC_, &src, 0, &bf, ULW_ALPHA);
    ReleaseDC(nullptr, screen);
}

int PopupMenu::HitTest(POINT client) const {
    for (size_t i = 0; i < itemRects_.size(); ++i) {
        const RECT& r = itemRects_[i];
        if (client.x >= r.left && client.x < r.right && client.y >= r.top &&
            client.y < r.bottom)
            return items_[i].separator ? -1 : (int)i;
    }
    return -1;
}

bool PopupMenu::PointInside(POINT screenPt) const {
    if (!hwnd_ || !visible_) return false;
    RECT wr{};
    GetWindowRect(hwnd_, &wr);
    return screenPt.x >= wr.left && screenPt.x < wr.right && screenPt.y >= wr.top &&
           screenPt.y < wr.bottom;
}

void PopupMenu::CloseSub() {
    if (sub_) {
        sub_->Hide();
    }
    openSub_ = -1;
}

void PopupMenu::OpenSub(int index) {
    if (index < 0 || index >= (int)items_.size()) return;
    const PopupMenuItem& it = items_[index];
    if (!it.hasSub()) {
        CloseSub();
        return;
    }
    if (openSub_ == index && sub_ && sub_->visible()) return;

    if (!sub_) {
        sub_ = new PopupMenu();
        if (!sub_->Create()) {
            delete sub_;
            sub_ = nullptr;
            return;
        }
        sub_->owner_ = owner_;
        // 二级菜单点中后，同样往上报，然后整串关掉
        sub_->onCommand = [this](int id) {
            if (onCommand) onCommand(id);
            Hide();
        };
    }

    openSub_ = index;
    RECT wr{};
    GetWindowRect(hwnd_, &wr);
    const RECT& ir = itemRects_[index];
    POINT pt{wr.left + ir.right - kShadow - 2, wr.top + ir.top - kShadow - kPadV};

    // 右边放不下就翻到左侧
    int needW = 180;
    {
        HDC screen = GetDC(nullptr);
        Graphics probe(screen);
        FontFamily fam(L"Microsoft YaHei UI");
        Font font(&fam, 13.0f, FontStyleRegular, UnitPixel);
        int maxText = 40;
        for (const auto& s : it.sub) {
            if (s.separator) continue;
            RectF box;
            probe.MeasureString(s.text.c_str(), -1, &font, PointF(0, 0), &box);
            maxText = std::max(maxText, (int)std::ceil(box.Width));
        }
        ReleaseDC(nullptr, screen);
        needW = kPadL + maxText + kPadR + kShadow * 2;
    }
    POINT cursor{};
    GetCursorPos(&cursor);
    MONITORINFO mi{};
    mi.cbSize = sizeof(mi);
    RECT limit{0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN)};
    if (GetMonitorInfoW(MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST), &mi)) limit = mi.rcWork;
    if (pt.x + needW > limit.right) pt.x = wr.left - needW + kShadow * 2 + 2;

    sub_->Show(pt, it.sub);
}

void PopupMenu::Invoke(int index) {
    if (index < 0 || index >= (int)items_.size()) return;
    const PopupMenuItem& it = items_[index];
    if (it.separator || it.disabled) return;
    if (it.hasSub()) {
        OpenSub(index);
        return;
    }
    const int id = it.id;
    Hide();
    if (onCommand && id > 0) onCommand(id);
}

void PopupMenu::Show(POINT screenPt, std::vector<PopupMenuItem> items) {
    if (!Create()) return;
    CloseSub();
    items_ = std::move(items);
    hover_ = -1;
    Layout();
    EnsureBuffer(w_, h_);
    if (!bufW_) return;
    Paint();

    // 钳制到工作区，避免菜单跑出屏幕
    MONITORINFO mi{};
    mi.cbSize = sizeof(mi);
    RECT limit{0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN)};
    if (GetMonitorInfoW(MonitorFromPoint(screenPt, MONITOR_DEFAULTTONEAREST), &mi))
        limit = mi.rcWork;

    int x = screenPt.x;
    int y = screenPt.y;
    if (x + w_ > limit.right) x = limit.right - w_;
    if (y + h_ > limit.bottom) y = limit.bottom - h_;
    if (x < limit.left) x = limit.left;
    if (y < limit.top) y = limit.top;

    SetWindowPos(hwnd_, HWND_TOPMOST, x, y, w_, h_, SWP_NOACTIVATE | SWP_SHOWWINDOW);
    visible_ = true;
    Present();
    // 没有前台窗口收不到 WM_ACTIVATE(WA_INACTIVE)，点外面就关不掉；
    // 拿不到前台时靠 WM_TIMER 兜底
    SetForegroundWindow(hwnd_);
    SetTimer(hwnd_, kWatchTimer, 80, nullptr);
}

void PopupMenu::Hide() {
    CloseSub();
    if (hwnd_ && visible_) {
        KillTimer(hwnd_, kWatchTimer);
        ShowWindow(hwnd_, SW_HIDE);
        visible_ = false;
        hover_ = -1;
        if (owner_ && IsWindow(owner_)) SetForegroundWindow(owner_);
    }
}

LRESULT CALLBACK PopupMenu::WndProcStatic(HWND h, UINT m, WPARAM w, LPARAM l) {
    PopupMenu* self = reinterpret_cast<PopupMenu*>(GetWindowLongPtrW(h, GWLP_USERDATA));
    if (m == WM_NCCREATE) {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(l);
        self = reinterpret_cast<PopupMenu*>(cs->lpCreateParams);
        SetWindowLongPtrW(h, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    }
    if (self) return self->WndProc(h, m, w, l);
    return DefWindowProcW(h, m, w, l);
}

LRESULT PopupMenu::WndProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_MOUSEACTIVATE:
        return MA_ACTIVATE;

    case WM_MOUSEMOVE: {
        POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        const int hit = HitTest(pt);
        if (hit != hover_) {
            hover_ = hit;
            Paint();
            Present();
        }
        if (hit >= 0) {
            if (items_[hit].hasSub()) {
                OpenSub(hit);
            } else {
                CloseSub();
            }
        }
        // 没有 TrackMouseEvent 的话，鼠标移出后 hover 会一直留着
        TRACKMOUSEEVENT tme{sizeof(tme), TME_LEAVE, h, 0};
        TrackMouseEvent(&tme);
        return 0;
    }

    case WM_MOUSELEAVE: {
        if (hover_ != -1) {
            hover_ = -1;
            Paint();
            Present();
        }
        return 0;
    }

    case WM_LBUTTONDOWN: {
        POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        const int hit = HitTest(pt);
        if (hit >= 0) {
            // 等鼠标抬起再执行，手感更像系统菜单
            return 0;
        }
        Hide();
        return 0;
    }

    case WM_LBUTTONUP: {
        POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        const int hit = HitTest(pt);
        if (hit >= 0) Invoke(hit);
        return 0;
    }

    case WM_RBUTTONUP:
        Hide();
        return 0;

    case WM_KEYDOWN:
        if (wp == VK_ESCAPE) {
            Hide();
            return 0;
        }
        if (wp == VK_DOWN || wp == VK_UP) {
            const int n = (int)items_.size();
            if (n > 0) {
                int i = hover_;
                for (int k = 0; k < n; ++k) {
                    i = (wp == VK_DOWN) ? (i + 1) : (i - 1);
                    if (i < 0) i = n - 1;
                    if (i >= n) i = 0;
                    if (!items_[i].separator && !items_[i].disabled) break;
                }
                hover_ = i;
                if (items_[i].hasSub()) OpenSub(i);
                else CloseSub();
                Paint();
                Present();
            }
            return 0;
        }
        if (wp == VK_RETURN && hover_ >= 0) {
            Invoke(hover_);
            return 0;
        }
        return 0;

    case WM_ACTIVATE: {
        if (LOWORD(wp) == WA_INACTIVE) {
            // 打开二级菜单时子菜单会抢走前台，那不是真的「点到外面去了」
            if (sub_ && sub_->visible()) return 0;
            POINT c{};
            GetCursorPos(&c);
            if (PointInside(c)) return 0;
            Hide();
        }
        return 0;
    }

    case WM_TIMER:
        if (wp == kWatchTimer) {
            POINT c{};
            GetCursorPos(&c);
            if (PointInside(c)) return 0;
            if (sub_ && sub_->visible() && sub_->PointInside(c)) return 0;
            // 鼠标离开整串菜单后，一旦按下任何键就把菜单收掉。
            // 只靠 WM_ACTIVATE 不可靠：菜单窗口有时拿不到前台（没有真实输入时
            // SetForegroundWindow 会被拒），那就永远等不到失活消息。
            if (GetAsyncKeyState(VK_LBUTTON) < 0 || GetAsyncKeyState(VK_RBUTTON) < 0 ||
                GetAsyncKeyState(VK_MBUTTON) < 0) {
                Hide();
            }
            return 0;
        }
        return 0;

    case WM_KILLFOCUS:
        return 0;

    case WM_NCHITTEST:
        return HTCLIENT;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

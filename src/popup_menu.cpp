#include "popup_menu.h"

#include "icon_render.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <cwchar>
#include <string>
#include <vector>

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
const int kShadow = 18;     // 阴影留白（越大影子铺得越开）
const int kRound = 8;       // 圆角
// 阴影的每一圈都是**实心圆角矩形**往外叠，所以同一像素会被好几圈覆盖 ——
// 单圈 alpha 必须压到极低。以前的算法（7-i、10 圈）会在卡片边缘聚成一条
// 4.5px 宽的深灰边，用户一直说「看着还是重」。
// 现在改成：圈数加到 18、每圈 alpha 只取 1~2，铺开 ~11px，
// 边缘叠出来的浓度约 7%，是一层「散开的淡影」而不是「一圈灰边」。
const int kShadowMin = 1;   // 单圈下限（0 就等于不画）
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

// 这个点所在显示器的工作区（没有就退回主屏尺寸）
RECT WorkAreaNear(POINT pt) {
    RECT limit{0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN)};
    MONITORINFO mi{};
    mi.cbSize = sizeof(mi);
    if (GetMonitorInfoW(MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST), &mi)) limit = mi.rcWork;
    return limit;
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

// ---------------------------------------------------------------------------
// 菜单文字：GDI + ClearType，直接画在缓冲上
//
// 这条走了三版，记一下为什么最后是它：
//
//   v1 GDI+ Graphics::DrawString —— 「有点发虚」。GDI+ 有自己的光栅器，
//      13px 汉字被摆到半像素位置，一条竖笔摊成两层淡影。
//   v2 LabelDib（黑底画白字 → 亮度当 alpha → 预乘合成）—— 峰值够了但整体发灰。
//      这套遮罩只有灰度，13px 中文的笔画太细，峰值压在 ~60% 上，整片灰乎乎
//      加 1~2px 晕边，用户原话「像用水笔写完字又用手涂抹了一样」。
//      （离线复现同条件：墨迹里只有 32% 的像素达到接近纯黑。）
//   v3 现在：菜单卡片是**不透明**的（bg alpha = 255），可以直接用 ClearType
//      在真实底色上画字 —— 笔画实心、边缘用亚像素锐化，和系统菜单同一档
//      （离线同条件对比：深墨占比 32% → 42%）。
//
// 两个前提，任一不成立就必须退回 v2：
//   1. 文字盒必须完全落在卡片的**不透明区**内。菜单项左边从 kPadL-4 = 26 起、
//      上下各留 (kItemH - 行高)/2 ≈ 6，都比圆角半径 kRound = 8 大，所以碰不到
//      圆角，也不会压到阴影。改 kPadL / kItemH / kRound 时要重新核一遍。
//   2. GDI 画完要把这一块的 alpha 补回 255。实测 GDI 在 32bpp DIB 上画字会把
//      墨迹像素的 alpha 写成 0（minA = 0），不补的话分层窗口上那块直接变
//      透明空洞 —— 文字成了一个个白洞。
// ---------------------------------------------------------------------------
const int kTextPx = 13;

HFONT MenuFont() {
    static HFONT f = nullptr;
    if (f) return f;
    LOGFONTW lf{};
    lf.lfHeight = -kTextPx;
    lf.lfWeight = FW_NORMAL;
    // 只有这里的 ClearType 是安全的：目标是不透明卡片。
    // 图标标题压在壁纸上半透明，那边仍走 ANTIALIASED（见 icon_render.cpp）。
    lf.lfQuality = CLEARTYPE_QUALITY;
    lstrcpynW(lf.lfFaceName, L"Microsoft YaHei UI", LF_FACESIZE);
    f = CreateFontIndirectW(&lf);
    return f;
}

int MenuTextWidth(const std::wstring& s) {
    if (s.empty()) return 0;
    HDC dc = GetDC(nullptr);
    if (!dc) return (int)s.size() * (kTextPx / 2);
    HGDIOBJ old = SelectObject(dc, MenuFont());
    SIZE sz{};
    GetTextExtentPoint32W(dc, s.c_str(), (int)s.size(), &sz);
    SelectObject(dc, old);
    ReleaseDC(nullptr, dc);
    return sz.cx;
}

// 行高（tmHeight）。文字盒按「行高」给，再按 (kItemH - 行高)/2 往下让，
// 就是行内垂直居中 —— 和上一版（LabelDib）的算法一致，
// 换栅格化方式时文字位置不会跟着跳一行。
int MenuFontHeight() {
    static int h = 0;
    if (h) return h;
    h = kTextPx + 2;
    HDC dc = GetDC(nullptr);
    if (dc) {
        HGDIOBJ old = SelectObject(dc, MenuFont());
        TEXTMETRICW tm{};
        if (GetTextMetricsW(dc, &tm)) h = tm.tmHeight;
        SelectObject(dc, old);
        ReleaseDC(nullptr, dc);
    }
    return h;
}

// GDI 画字会把这一块像素的 alpha 清成 0，画完必须整块补回 255。
// 只补文字盒：它整个落在卡片的不透明区内，这些像素本来就该是 255，
// 所以「无条件设 255」不会改坏任何东西。
void RestoreAlpha(void* bits, int bufW, int bufH, const RECT& r) {
    if (!bits || bufW <= 0 || bufH <= 0) return;
    const int x0 = (std::max)(0, (int)r.left);
    const int x1 = (std::min)(bufW, (int)r.right);
    const int y0 = (std::max)(0, (int)r.top);
    const int y1 = (std::min)(bufH, (int)r.bottom);
    auto* p = reinterpret_cast<BYTE*>(bits);
    for (int y = y0; y < y1; ++y) {
        BYTE* row = p + (size_t)y * bufW * 4;
        for (int x = x0; x < x1; ++x) row[x * 4 + 3] = 255;
    }
}

// GDI+ 的 ARGB → GDI 的 COLORREF
COLORREF FromArgb(ARGB c) {
    return RGB((c >> 16) & 0xFF, (c >> 8) & 0xFF, c & 0xFF);
}

// GDI+ 块里只记账，等 Flush 完再统一画（GDI 不认 GDI+ 的延迟绘制）
struct MenuTextOut {
    const std::wstring* text;
    COLORREF color;
    int maxW;
    int boxLeft;
    int boxTop;
};

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
    // 宽度必须用**和画字同一套 GDI 字体**来量。以前这里走 GDI+ 的 MeasureString，
    // 换成 GDI 栅格化之后两边字宽会差 1~2px —— 差的那点就够让最后一行文字
    // 打上省略号，或者右边空出一条。量宽和画字从此共用 MenuFont()。
    int maxText = 40;
    for (const auto& it : items_) {
        if (it.separator) continue;
        // +2 和 LabelDib 内部「量宽后 boxW = best + 2」对齐，保证绝不打省略号
        maxText = std::max(maxText, MenuTextWidth(it.text) + 2);
    }

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

    // 文字不进 GDI+（见 MenuText 的说明），先在下面那个块里记账，
    // 等 Graphics / Bitmap 都出了作用域再统一贴 —— 顺序反了文字会被
    // 上面那层卡片底色盖住。
    std::vector<MenuTextOut> textOut;
    textOut.reserve(items_.size());

    {
        Bitmap target(bufW_, bufH_, bufW_ * 4, PixelFormat32bppPARGB,
                      reinterpret_cast<BYTE*>(bits_));
        Graphics g(&target);
        g.SetSmoothingMode(SmoothingModeAntiAlias);
        g.SetPixelOffsetMode(PixelOffsetModeHighQuality);

        const RectF body((float)kShadow, (float)kShadow, (float)contentW_, (float)contentH_);

        // 阴影：由外到内叠几层，越外越淡。
        // 圈数多、单圈极淡 → 出来的是一层散开的柔影；只有最里面 4 圈给到 2，
        // 让贴边的地方稍微实一点，不然卡片像浮在空气里。
        for (int i = kShadow; i >= 1; --i) {
            const int a = (i <= 4) ? 2 : kShadowMin;
            GraphicsPath sp;
            AddRoundRect(sp, RectF(body.X - i * 0.6f, body.Y - i * 0.5f + 2.0f,
                                   body.Width + i * 1.2f, body.Height + i * 1.0f),
                         kRound + i * 0.35f);
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

            // 勾选标记：对勾
            if (it.checked && !it.separator) {
                Pen cp(accent, 1.9f);
                cp.SetStartCap(LineCapRound);
                cp.SetEndCap(LineCapRound);
                const float cx = (float)r.left + 15.0f;
                g.DrawLine(&cp, cx - 4.0f, cy, cx - 1.0f, cy + 3.5f);
                g.DrawLine(&cp, cx - 1.0f, cy + 3.5f, cx + 5.0f, cy - 4.0f);
            }

            // 文字只记账。排版盒和以前一样：左边从 kPadL - 4 起（给勾选标记让位），
            // 右边留 kPadR（给子菜单箭头让位）。
            textOut.push_back(MenuTextOut{&it.text, FromArgb(it.disabled ? fgDim : fg),
                                          r.right - r.left - kPadL - kPadR + 4,
                                          r.left + kPadL - 4, r.top});

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

    // GDI 文字：ClearType 直接画在缓冲上（为什么不用 LabelDib 抠 alpha，
    // 见 MenuFont 上面那段注释）。必须在 GDI+ 全部出作用域之后，
    // 否则 GDI 写进去的像素会被 GDI+ 的延迟绘制盖掉。
    HGDIOBJ oldFont = SelectObject(memDC_, MenuFont());
    const int oldMode = SetBkMode(memDC_, TRANSPARENT);
    for (const auto& t : textOut) {
        SetTextColor(memDC_, t.color);
        // 自己算垂直位置而不用 DT_VCENTER：DT_VCENTER 的取整和上一版
        // 的 (kItemH - 行高)/2 可能差 1px，换了栅格化方式不该让字跟着上下跳。
        const int ch = MenuFontHeight();
        const int top = t.boxTop + (kItemH - ch) / 2;
        RECT rc{t.boxLeft, top, t.boxLeft + t.maxW, top + ch};
        // 超宽靠 DT_END_ELLIPSIS（以前是 LabelDib 自己打省略号）
        DrawTextW(memDC_, t.text->c_str(), -1, &rc,
                  DT_LEFT | DT_TOP | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);
        RestoreAlpha(bits_, bufW_, bufH_, rc);
    }
    SetBkMode(memDC_, oldMode);
    SelectObject(memDC_, oldFont);
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

    // 右边放不下就翻到左侧。宽度和 Layout() 用同一套量法（GDI 字体），
    // 以前这里是 GDI+ MeasureString，两边差几像素就会翻错边。
    int needW = 180;
    {
        int maxText = 40;
        for (const auto& s : it.sub) {
            if (s.separator) continue;
            maxText = std::max(maxText, MenuTextWidth(s.text) + 2);
        }
        needW = kPadL + maxText + kPadR + kShadow * 2;
    }
    POINT cursor{};
    GetCursorPos(&cursor);
    const RECT limit = WorkAreaNear(pt);
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
    // 先不通知「关闭」：命令跑完再通知，宿主才知道「菜单关了、这单点了什么」。
    // 顺序反了的话，宿主的 onClose 会先把盒子收起来，紧接着命令又把它展开
    // （重命名类命令要占住盒子），视觉上闪一下。
    Hide(false);
    if (onCommand && id > 0) onCommand(id);
    NotifyClosed();
}

void PopupMenu::NotifyClosed() {
    if (!onClose) return;
    auto cb = std::move(onClose);
    onClose = nullptr;   // 只触发一次，别串到下一个菜单
    cb();
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
    const RECT limit = WorkAreaNear(screenPt);
    int x = screenPt.x;
    int y = screenPt.y;
    if (x + w_ > limit.right) x = limit.right - w_;
    if (y + h_ > limit.bottom) y = limit.bottom - h_;
    if (x < limit.left) x = limit.left;
    if (y < limit.top) y = limit.top;

    PresentAt(x, y);
}

void PopupMenu::ShowAnchoredToRect(const RECT& anchor, std::vector<PopupMenuItem> items) {
    if (!Create()) return;
    CloseSub();
    items_ = std::move(items);
    hover_ = -1;
    Layout();
    EnsureBuffer(w_, h_);
    if (!bufW_) return;
    Paint();

    const POINT mid{(anchor.left + anchor.right) / 2, (anchor.top + anchor.bottom) / 2};
    const RECT limit = WorkAreaNear(mid);

    // 卡片贴在 anchor 的右下角。窗口本身四周留了 kShadow 的阴影边距，
    // 所以窗口坐标要往回让同样的距离，卡片的角才正好压框角上。
    int x = anchor.right - kShadow;
    int y = anchor.bottom - kShadow;
    // 右边放不下 → 翻到框的左边（先试框外，框外也不够就贴着工作区右缘）
    if (x + w_ > limit.right) x = anchor.left - w_ + kShadow;
    // 下面放不下 → 往上贴框底（框很矮时再统一夹进工作区）
    if (y + h_ > limit.bottom) y = anchor.bottom - h_ + kShadow;
    // 阴影可以出血，卡片不能：只夹到「卡片本身还在屏幕里」
    if (x < limit.left - kShadow) x = limit.left - kShadow;
    if (y < limit.top - kShadow) y = limit.top - kShadow;
    if (x + w_ > limit.right) x = limit.right - w_;
    if (y + h_ > limit.bottom) y = limit.bottom - h_;

    PresentAt(x, y);
}

void PopupMenu::PresentAt(int x, int y) {
    SetWindowPos(hwnd_, HWND_TOPMOST, x, y, w_, h_, SWP_NOACTIVATE | SWP_SHOWWINDOW);
    visible_ = true;
    Present();
    // 没有前台窗口收不到 WM_ACTIVATE(WA_INACTIVE)，点外面就关不掉；
    // 拿不到前台时靠 WM_TIMER 兜底
    SetForegroundWindow(hwnd_);
    SetTimer(hwnd_, kWatchTimer, 80, nullptr);
}

void PopupMenu::Hide(bool notify) {
    CloseSub();
    if (hwnd_ && visible_) {
        KillTimer(hwnd_, kWatchTimer);
        ShowWindow(hwnd_, SW_HIDE);
        visible_ = false;
        hover_ = -1;
        if (owner_ && IsWindow(owner_)) SetForegroundWindow(owner_);
        if (notify) NotifyClosed();
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

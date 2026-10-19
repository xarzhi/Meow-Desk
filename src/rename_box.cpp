#include "rename_box.h"

#include <algorithm>
#include <cstring>
#include <utility>

#include <windowsx.h>
#include <commctrl.h>   // SetWindowSubclass / DefSubclassProc

// GDI+ 的头文件顺序：Image / PropertyItem 需要 IStream / PROPID
#include <objidl.h>
#include <propidl.h>
#include <gdiplus.h>

#include "icon_render.h"

using namespace Gdiplus;

namespace RenameBox {
namespace {

const wchar_t* kClass = L"MeowDeskRenameField";

// ---- 几何（像素）----
const int kShadow = 10;   // 阴影留白：客户区比输入格每边大这么多
const int kRound = 4;     // 圆角（贴着图标，比卡片小一号）
const int kPadX = 6;      // 输入格左右内缩：EDIT 比底槽窄，字不贴边框
const int kPadY = 3;      // 输入格上下内边距
const int kMinW = 72;     // 再窄就没法打字了
const int kMaxW = 320;    // 长名字最多撑这么宽，别盖住旁边的图标

// 「焦点跑掉了没有」的延迟检查（见 EditProc 的 WM_KILLFOCUS）
const UINT kMsgCheckFocus = WM_APP + 12;
// 「点到别处了」的延后提交（见 MouseProc）
const UINT kMsgCommitElsewhere = WM_APP + 13;

HWND g_card = nullptr;
HWND g_edit = nullptr;
std::function<void(const std::wstring&)> g_cb;
std::function<void()> g_done;   // 收尾（提交 / 取消都调一次）
bool g_activated = false;   // 已经真的被激活过一次（否则第一次 WA_INACTIVE 是假的）
bool g_dark = false;

// 布局结果
int g_lineH = 17;      // 标签字体行高
int g_textH = 17;      // 内容高度（一行或两行）
int g_editH = 19;      // EDIT 本体高度（一行 + 2px，免得降部被切）
int g_fieldW = 0;      // 输入格宽高
int g_fieldH = 0;
RECT g_rcField{};      // 客户区里的输入格（自己画的圆角底槽）
RECT g_rcEdit{};       // 客户区里的 EDIT 窗口位置
int g_bufW = 0;
int g_bufH = 0;

HDC g_memDC = nullptr;
HBITMAP g_dib = nullptr;
void* g_bits = nullptr;

HBRUSH g_editBgDark = nullptr;   // 深色主题下输入框的底色（进程内建一次就够）
HHOOK g_mouseHook = nullptr;     // 改名期间临时挂的线程级鼠标钩子（见 MouseProc）

void Finish(bool commit);

// ---------------------------------------------------------------------------
// 「点到别处 → 提交」
//
// 桌面层和盒子都是 WS_EX_NOACTIVATE，点它们根本不会把焦点从输入框抢走，
// 所以只靠 WM_KILLFOCUS 判断「改名结束」永远等不到。这里在改名期间挂一个
// **线程级**鼠标钩子：只收本线程窗口的点击（点别的程序照样走 killfocus 那条路），
// 落到卡片和输入框以外就**延后**提交。
//
// 为什么是延后（PostMessage）而不是当场 Finish：Finish 会走 CommitRename →
// ReloadDesktopIcons，把 g_icons 里的图标全析构重建。当场做就等于在
// 「别人正在处理这一次点击」的消息里把它的图标对象拆了。
// ---------------------------------------------------------------------------
LRESULT CALLBACK MouseProc(int code, WPARAM wp, LPARAM lp) {
    if (code == HC_ACTION && wp == WM_LBUTTONDOWN && g_card) {
        const auto* ms = reinterpret_cast<const MSLLHOOKSTRUCT*>(lp);
        HWND under = WindowFromPoint(ms->pt);
        const bool inside = (under == g_card) || (under == g_edit) ||
                            (under && GetAncestor(under, GA_ROOT) == g_card);
        if (!inside) PostMessageW(g_card, kMsgCommitElsewhere, 0, 0);
    }
    return CallNextHookEx(g_mouseHook, code, wp, lp);
}

void InstallMouseHook() {
    if (g_mouseHook) return;
    g_mouseHook = SetWindowsHookExW(WH_MOUSE, MouseProc, nullptr, GetCurrentThreadId());
}

void RemoveMouseHook() {
    if (!g_mouseHook) return;
    UnhookWindowsHookEx(g_mouseHook);
    g_mouseHook = nullptr;
}

// ---------------------------------------------------------------------------
// 字体
// ---------------------------------------------------------------------------
// 输入框用**图标标签同一套字体**：就地改名时新打的字和原来的字应该长得一样，
// 只是更清晰（EDIT 底下是不透明的白/深底，可以用 ClearType）。
HFONT FieldFont() {
    static HFONT cached = nullptr;
    if (cached) return cached;
    LOGFONTW lf{};
    if (GetObjectW(IconLabelFont(), sizeof(lf), &lf) == 0) {
        lf.lfHeight = -12;
        lf.lfWeight = FW_NORMAL;
        wcscpy_s(lf.lfFaceName, L"Microsoft YaHei UI");
    }
    lf.lfQuality = CLEARTYPE_QUALITY;
    cached = CreateFontIndirectW(&lf);
    return cached;
}

int MeasureTextW(const std::wstring& s) {
    HDC dc = CreateCompatibleDC(nullptr);
    if (!dc) return 80;
    HGDIOBJ of = SelectObject(dc, FieldFont());
    SIZE sz{};
    GetTextExtentPoint32W(dc, s.c_str(), (int)s.size(), &sz);
    SelectObject(dc, of);
    DeleteDC(dc);
    return (int)sz.cx;
}

// ---------------------------------------------------------------------------
// 配色：和确认框 / 弹出菜单 / 盒子同一套，切深色主题时不出戏
// ---------------------------------------------------------------------------
bool LightTheme() {
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

// EDIT 的底色（深色主题下自己给，否则就是系统白）
HBRUSH EditBgBrush() {
    if (g_dark) {
        if (!g_editBgDark) g_editBgDark = CreateSolidBrush(RGB(35, 35, 41));
        return g_editBgDark;
    }
    return (HBRUSH)GetStockObject(WHITE_BRUSH);
}

void AddRoundRect(GraphicsPath& p, const RectF& r, float rad) {
    if (rad <= 0.0f) {
        p.AddRectangle(r);
        return;
    }
    if (rad * 2.0f > r.Width) rad = r.Width * 0.5f;
    if (rad * 2.0f > r.Height) rad = r.Height * 0.5f;
    p.AddArc(r.X, r.Y, rad * 2, rad * 2, 180.0f, 90.0f);
    p.AddArc(r.X + r.Width - rad * 2, r.Y, rad * 2, rad * 2, 270.0f, 90.0f);
    p.AddArc(r.X + r.Width - rad * 2, r.Y + r.Height - rad * 2, rad * 2, rad * 2, 0.0f, 90.0f);
    p.AddArc(r.X, r.Y + r.Height - rad * 2, rad * 2, rad * 2, 90.0f, 90.0f);
    p.CloseFigure();
}

// ---------------------------------------------------------------------------
// 布局
// ---------------------------------------------------------------------------
void Layout(const RECT& target, const std::wstring& name) {
    g_lineH = IconLabelLineHeight();
    g_editH = g_lineH + 2;

    const int targetH = (int)(target.bottom - target.top);
    const int targetW = (int)(target.right - target.left);

    // 只区分「一行」和「两行」：标签框够两行时输入框也占两行，
    // 否则选中态下展开的第二行会从输入框底下露出来。
    g_textH = (targetH >= g_lineH * 2 - 2) ? g_lineH * 2 : g_lineH;

    // 宽度：至少盖住原来的名字，长名字可以再宽一点（最多 kMaxW）
    int want = std::max(targetW, MeasureTextW(name) + kPadX * 2 + 8);
    g_fieldW = std::max(kMinW, std::min(want, kMaxW));
    g_fieldH = g_textH + kPadY * 2;

    g_bufW = g_fieldW + kShadow * 2;
    g_bufH = g_fieldH + kShadow * 2;

    g_rcField = RECT{kShadow, kShadow, kShadow + g_fieldW, kShadow + g_fieldH};

    // EDIT 只有一行高，摆在第一行文字的位置上（不是输入格正中）：
    // 标签允许两行时，输入格是两行高，但光标得停在原来那行字上。
    const int innerTop = kPadY + std::max(0, (targetH - g_textH) / 2);
    g_rcEdit = RECT{kShadow + kPadX, kShadow + innerTop, kShadow + g_fieldW - kPadX,
                    kShadow + innerTop + g_editH};
}

void EnsureBuffer() {
    // 尺寸每次 Show 都可能变（名字长短不同 → 输入格宽度不同），所以按尺寸重建
    static int lastW = 0, lastH = 0;
    if (g_memDC && lastW == g_bufW && lastH == g_bufH) return;
    if (g_memDC) {
        DeleteDC(g_memDC);
        g_memDC = nullptr;
    }
    if (g_dib) {
        DeleteObject(g_dib);
        g_dib = nullptr;
    }
    g_bits = nullptr;
    if (CreateDib(g_bufW, g_bufH, g_memDC, g_dib, g_bits)) {
        lastW = g_bufW;
        lastH = g_bufH;
    } else {
        lastW = lastH = 0;
    }
}

// ---------------------------------------------------------------------------
// 绘制 / 上屏
// ---------------------------------------------------------------------------
void Paint() {
    if (!g_memDC || !g_bits || g_bufW <= 0 || g_bufH <= 0) return;
    std::memset(g_bits, 0, (size_t)g_bufW * (size_t)g_bufH * 4);

    const bool light = !g_dark;
    const ARGB bg = light ? 0xFFFFFFFF : 0xFF232329;
    const ARGB border = light ? 0xFFC6C6CC : 0xFF55555E;

    {
        Bitmap target(g_bufW, g_bufH, g_bufW * 4, PixelFormat32bppPARGB,
                      reinterpret_cast<BYTE*>(g_bits));
        Graphics g(&target);
        g.SetSmoothingMode(SmoothingModeAntiAlias);
        g.SetPixelOffsetMode(PixelOffsetModeHighQuality);
        // 分层窗口 + alpha：不能用 ClearType（会出彩色杂边）。真正的字在 EDIT 里。
        g.SetTextRenderingHint(TextRenderingHintAntiAlias);

        const RectF body((float)g_rcField.left, (float)g_rcField.top, (float)g_fieldW,
                         (float)g_fieldH);

        // 阴影：由外到内叠几层，越外越淡。这些圈是实心圆角矩形往外叠，
        // 同一像素会被好几圈覆盖，所以单圈 alpha 必须压得很低。
        for (int i = kShadow; i >= 1; --i) {
            const int a = (i > kShadow - 3) ? 2 : 1;
            GraphicsPath sp;
            AddRoundRect(sp,
                         RectF(body.X - i * 0.5f, body.Y - i * 0.4f + 2.0f,
                               body.Width + i * 1.0f, body.Height + i * 0.9f),
                         kRound + i * 0.4f);
            SolidBrush sb{Color((BYTE)a, 0, 0, 0)};
            g.FillPath(&sb, &sp);
        }

        GraphicsPath field;
        AddRoundRect(field, body, (float)kRound);
        SolidBrush fb(bg);
        g.FillPath(&fb, &field);
        Pen pen(border, 1.0f);
        g.DrawPath(&pen, &field);

        g.Flush(FlushIntentionSync);
    }
}

void Present() {
    if (!g_card || !g_memDC) return;
    BLENDFUNCTION bf{};
    bf.BlendOp = AC_SRC_OVER;
    bf.SourceConstantAlpha = 255;
    bf.AlphaFormat = AC_SRC_ALPHA;

    RECT wr{};
    GetWindowRect(g_card, &wr);
    HDC screen = GetDC(nullptr);
    POINT dst{wr.left, wr.top};
    SIZE size{g_bufW, g_bufH};
    POINT src{0, 0};
    UpdateLayeredWindow(g_card, screen, &dst, &size, g_memDC, &src, 0, &bf, ULW_ALPHA);
    ReleaseDC(nullptr, screen);
}

// ---------------------------------------------------------------------------
// 提交 / 收尾
// ---------------------------------------------------------------------------
void Finish(bool commit) {
    if (!g_card) return;

    std::wstring text;
    if (commit && g_edit) {
        // 用 WM_GETTEXT 而不是 GetWindowTextW：输入框里的文字也可能是自动化脚本
        // 通过 WM_SETTEXT 写进来的，让 EDIT 自己回答更可靠
        wchar_t buf[512]{};
        SendMessageW(g_edit, WM_GETTEXT, (WPARAM)511, (LPARAM)buf);
        text = buf;
        size_t a = 0, b = text.size();
        while (a < b && (text[a] == L' ' || text[a] == L'\t')) ++a;
        while (b > a && (text[b - 1] == L' ' || text[b - 1] == L'\t')) --b;
        text = text.substr(a, b - a);
    }

    // 先把状态清干净、再销毁窗口：销毁过程中还会收到 WM_KILLFOCUS / WM_ACTIVATE，
    // 不清的话会递归回到这里。
    HWND card = g_card;
    HWND edit = g_edit;
    g_card = nullptr;
    g_edit = nullptr;
    g_activated = false;
    RemoveMouseHook();   // 窗口要没了，钩子留着会往野句柄上 PostMessage
    auto cb = std::move(g_cb);
    auto done = std::move(g_done);
    g_cb = nullptr;
    g_done = nullptr;

    if (edit) DestroyWindow(edit);   // 先拆被拥有的那个，再拆卡片
    if (card) DestroyWindow(card);

    // 回调放在窗口销毁之后：里面会重新枚举图标、可能再弹别的窗口，
    // 不能在这些消息处理过程中把窗口拆掉。
    if (commit && !text.empty() && cb) cb(text);
    if (done) done();
}

// ---------------------------------------------------------------------------
// 消息处理
// ---------------------------------------------------------------------------
LRESULT CALLBACK EditProc(HWND h, UINT m, WPARAM w, LPARAM l, UINT_PTR, DWORD_PTR) {
    switch (m) {
    case WM_KEYDOWN:
        if (w == VK_RETURN) {
            Finish(true);
            return 0;
        }
        if (w == VK_ESCAPE) {
            Finish(false);
            return 0;
        }
        break;
    case WM_CHAR:
        // 单行 EDIT 收到回车会「叮」一声，Esc 也会 —— 已经自己处理掉了就别再响
        if (w == L'\r' || w == L'\n' || w == 27) return 0;
        break;
    case WM_GETDLGCODE:
        return DLGC_WANTALLKEYS;
    case WM_SETFOCUS:
        // 真正拿到键盘焦点才算「已经立住」；卡片本身可能一直没被激活过
        g_activated = true;
        break;
    case WM_KILLFOCUS:
        // 焦点可能刚跑到卡片上（同一线程），也可能跑到别的程序。
        // 这里不能立刻判断：此刻新窗口的前台资格还没定下来，等这一轮消息走完再看。
        if (g_card) PostMessageW(g_card, kMsgCheckFocus, 0, 0);
        break;
    case WM_NCDESTROY:
        RemoveWindowSubclass(h, EditProc, 1);
        break;
    }
    return DefSubclassProc(h, m, w, l);
}

LRESULT CALLBACK CardProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
    case kMsgCheckFocus:
        // 点到卡片自己 = 还在改名；跑到别的程序 = 这一轮改名结束，
        // 按资源管理器的习惯**提交**（不是放弃）。
        if (g_card && g_activated) {
            HWND fg = GetForegroundWindow();
            if (fg != g_card && fg != g_edit) Finish(true);
        }
        return 0;

    case kMsgCommitElsewhere:
        // 点在自己桌面/盒子/别处 → 提交（延后到这里才真做，见 MouseProc 的注释）
        Finish(true);
        return 0;

    case WM_ACTIVATE:
        if (LOWORD(w) != WA_INACTIVE) g_activated = true;
        return 0;

    case WM_MOUSEACTIVATE:
        return MA_ACTIVATE;

    case WM_LBUTTONDOWN:
        // 点在输入格的边距上：这一下已经把焦点从输入框抢走了，还给输入框
        if (g_edit) SetFocus(g_edit);
        return 0;

    case WM_CTLCOLOREDIT: {
        // 输入框是卡片「拥有」的顶层 EDIT，这条消息能不能到这里取决于系统，
        // 到了就顺便把深色主题的底色和字色改掉。
        HDC dc = (HDC)w;
        SetTextColor(dc, g_dark ? RGB(240, 240, 244) : RGB(27, 27, 31));
        SetBkColor(dc, g_dark ? RGB(35, 35, 41) : RGB(255, 255, 255));
        return (LRESULT)EditBgBrush();
    }

    case WM_ERASEBKGND:
        return 1;   // 内容全在分层位图里，别让 GDI 再刷一遍

    case WM_CLOSE:
        Finish(false);
        return 0;

    default:
        break;
    }
    return DefWindowProcW(h, m, w, l);
}

void EnsureCardClass() {
    static bool done = false;
    if (done) return;
    done = true;
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = CardProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = kClass;
    RegisterClassExW(&wc);
}

}  // namespace

// ---------------------------------------------------------------------------
// 对外接口
// ---------------------------------------------------------------------------
bool IsOpen() { return g_card != nullptr; }

void Close() { Finish(false); }

void Commit() { Finish(true); }

void ShowInPlace(const RECT& rect, const std::wstring& currentName,
                 std::function<void(const std::wstring&)> onCommit, Sel sel,
                 std::function<void()> onDone) {
    Close();   // 之前还开着一个就先按取消收掉
    if (rect.right <= rect.left || rect.bottom <= rect.top) {
        if (onDone) onDone();
        return;
    }

    EnsureGdiplus();
    EnsureCardClass();

    g_cb = std::move(onCommit);
    g_done = std::move(onDone);
    g_dark = !LightTheme();
    g_activated = false;

    Layout(rect, currentName);
    EnsureBuffer();
    if (!g_memDC) {
        g_cb = nullptr;
        auto done = std::move(g_done);
        g_done = nullptr;
        if (done) done();
        return;
    }

    // 摆位：水平居中于目标矩形，上沿对齐第一行文字；夹进所在显示器的工作区
    const int targetH = (int)(rect.bottom - rect.top);
    int x = (int)rect.left + ((int)(rect.right - rect.left) - g_fieldW) / 2;
    int y = (int)rect.top + std::max(0, (targetH - g_textH) / 2) - kPadY;

    MONITORINFO mi{};
    mi.cbSize = sizeof(mi);
    HMONITOR mon = MonitorFromRect(&rect, MONITOR_DEFAULTTONEAREST);
    RECT lim{0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN)};
    if (mon && GetMonitorInfoW(mon, &mi)) lim = mi.rcWork;
    if (x + g_bufW > (int)lim.right) x = (int)lim.right - g_bufW;
    if (y + g_bufH > (int)lim.bottom) y = (int)lim.bottom - g_bufH;
    if (x < (int)lim.left) x = (int)lim.left;
    if (y < (int)lim.top) y = (int)lim.top;

    g_card = CreateWindowExW(WS_EX_LAYERED | WS_EX_TOOLWINDOW | WS_EX_TOPMOST, kClass, L"",
                             WS_POPUP, x, y, g_bufW, g_bufH, nullptr, nullptr,
                             GetModuleHandleW(nullptr), nullptr);
    if (!g_card) {
        g_cb = nullptr;
        auto done = std::move(g_done);
        g_done = nullptr;
        if (done) done();
        return;
    }

    // 输入框为什么是「卡片拥有的顶层弹出窗口」而不是子窗口：
    // 卡片用 UpdateLayeredWindow 提供 BGRA 位图，这种窗口**不能**放子控件
    // （子窗口不会被合成上去）。做成 owned 窗口有两个好处：
    //   · owned 窗口永远压在 owner 上面 → 不用担心它被卡片盖住；
    //   · 它是真正的顶层 EDIT，键盘焦点、光标闪烁、拖选、输入法全部是系统原生的。
    g_edit = CreateWindowExW(WS_EX_TOOLWINDOW, L"EDIT", L"",
                             WS_POPUP | WS_VISIBLE | ES_AUTOHSCROLL, x + (int)g_rcEdit.left,
                             y + (int)g_rcEdit.top, (int)(g_rcEdit.right - g_rcEdit.left),
                             (int)(g_rcEdit.bottom - g_rcEdit.top), g_card, nullptr,
                             GetModuleHandleW(nullptr), nullptr);
    if (!g_edit) {
        Finish(false);
        return;
    }
    SendMessageW(g_edit, WM_SETFONT, (WPARAM)FieldFont(), TRUE);
    // 左右留白由「EDIT 比底槽窄一圈」保证（见 Layout），这里不用再加边距
    SendMessageW(g_edit, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN, MAKELPARAM(0, 0));
    SendMessageW(g_edit, EM_SETLIMITTEXT, 200, 0);
    SetWindowSubclass(g_edit, EditProc, 1, 0);
    SetWindowTextW(g_edit, currentName.c_str());

    // 预选：改文件名时只选主文件名（最后一个点之后算扩展名；点在最前面算隐藏文件名，
    // 不切），改盒子/分组名就全选。
    int selEnd = (int)currentName.size();
    if (sel == Sel::Stem) {
        const size_t dot = currentName.find_last_of(L'.');
        if (dot != std::wstring::npos && dot > 0) selEnd = (int)dot;
    }
    SendMessageW(g_edit, EM_SETSEL, 0, selEnd);
    SendMessageW(g_edit, EM_SCROLLCARET, 0, 0);

    SetWindowPos(g_card, HWND_TOPMOST, x, y, g_bufW, g_bufH, SWP_SHOWWINDOW | SWP_NOACTIVATE);
    Paint();
    Present();
    InstallMouseHook();   // 改名期间「点到别处就提交」

    // 输入框在卡片显示之后再显一次：它是「卡片拥有」的窗口，
    // owner 从隐藏变可见时它不一定跟着显示，漏这一步会「框出来了但没法打字」。
    ShowWindow(g_edit, SW_SHOWNA);
    SetWindowPos(g_edit, HWND_TOPMOST, x + (int)g_rcEdit.left, y + (int)g_rcEdit.top,
                 (int)(g_rcEdit.right - g_rcEdit.left), (int)(g_rcEdit.bottom - g_rcEdit.top),
                 SWP_SHOWWINDOW | SWP_NOACTIVATE);

    // 我们的主窗口都是 WS_EX_NOACTIVATE，进程得做一次「真正激活」这个弹窗，
    // 输入框才拿得到键盘焦点。右键菜单刚关掉，前台权利还在我们手上，一般都会成功。
    SetForegroundWindow(g_edit);
    SetActiveWindow(g_edit);
    SetFocus(g_edit);
}

}  // namespace RenameBox

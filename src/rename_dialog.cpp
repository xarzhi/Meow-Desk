#include "rename_dialog.h"

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

namespace RenameDialog {
namespace {

const wchar_t* kCardClass = L"MeowDeskRenameCard";

// ---- 几何（单位：像素，和 confirm_dialog 同一套口径）----
const int kShadow = 14;   // 阴影留白：客户区比卡片每边大这么多
const int kRound = 10;    // 卡片圆角
const int kCardW = 400;   // 卡片宽度（固定）
const int kPadX = 24;     // 卡片左右内边距
const int kPadTop = 22;   // 卡片上内边距
const int kTitleH = 22;   // 标题行高
const int kAfterTitle = 16;
const int kEditH = 32;    // 输入格（底槽）高度
const int kFieldPadX = 10;   // 输入格左右内缩：EDIT 比底槽窄一点，字不贴边框
const int kAfterEdit = 20;
const int kBtnW = 92;
const int kBtnH = 32;
const int kBtnGap = 10;
const int kPadBottom = 22;

// 「焦点跑掉了没有」的延迟检查（见 EditProc 的 WM_KILLFOCUS）
const UINT kMsgCheckFocus = WM_APP + 12;

HWND g_card = nullptr;
HWND g_edit = nullptr;
std::wstring g_title;
std::function<void(const std::wstring&)> g_cb;
// 收尾回调（提交/取消都会调一次）：主程序用它解除「改名期间别自动收起盒子」的占位
std::function<void()> g_onDone;
bool g_activated = false;   // 已经真的被激活过一次（否则第一次 WA_INACTIVE 是假的）
bool g_dark = false;
int g_hover = -1;           // 0 = 取消，1 = 确定，-1 = 空白
int g_press = -1;

int g_cardH = 0;
// g_rcField = 卡片上画出来的「输入格」（视觉上的那个框，圆角+描边）
// g_rcEdit  = 真正的 EDIT 窗口，只有「一行字」那么高，垂直居中嵌在 g_rcField 里。
//
// 为什么不让 EDIT 直接填满 g_rcField：单行 EDIT 是**按自己的字体行高从客户区顶部**
// 开始排字的，窗口给 32px 高、字只有 19px，字就会贴着上半截 —— 看起来没居中。
// 把窗口高度收成一行高、再整体居中摆进底槽，垂直居中就由几何保证了，
// 不用依赖 EDIT 的内部排版规则（那个规则在不同系统版本上还不完全一样）。
RECT g_rcField{};
RECT g_rcEdit{};
RECT g_rcOk{};
RECT g_rcCancel{};

HDC g_memDC = nullptr;
HBITMAP g_dib = nullptr;
void* g_bits = nullptr;
int g_bufW = 0;
int g_bufH = 0;

HBRUSH g_editBgDark = nullptr;   // 深色主题下输入框的底色（进程内建一次就够）

int CardHeight() {
    return kPadTop + kTitleH + kAfterTitle + kEditH + kAfterEdit + kBtnH + kPadBottom;
}

void Finish(bool commit);

// ---------------------------------------------------------------------------
// 配色 / 画法：和确认框、弹出菜单同一套，切深色主题时不出戏
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

HFONT DialogFont() {
    static HFONT cached = nullptr;
    if (cached) return cached;
    HDC dc = GetDC(nullptr);
    const int dpi = dc ? GetDeviceCaps(dc, LOGPIXELSY) : 96;
    if (dc) ReleaseDC(nullptr, dc);

    LOGFONTW lf{};
    lf.lfHeight = -MulDiv(14, dpi, 96);
    lf.lfWeight = FW_NORMAL;
    lf.lfCharSet = DEFAULT_CHARSET;
    lf.lfQuality = CLEARTYPE_QUALITY;
    wcscpy_s(lf.lfFaceName, L"Microsoft YaHei UI");
    cached = CreateFontIndirectW(&lf);
    return cached;
}

// 输入框字体的行高（tmHeight）。输入框窗口的高度就从这里推 ——
// 不能写死像素，换系统字号 / 高 DPI 会露半个字。
int FieldLineHeight() {
    static int cached = 0;
    if (cached > 0) return cached;
    HDC dc = CreateCompatibleDC(nullptr);
    if (!dc) return 20;
    HGDIOBJ of = SelectObject(dc, DialogFont());
    TEXTMETRICW tm{};
    GetTextMetricsW(dc, &tm);
    SelectObject(dc, of);
    DeleteDC(dc);
    cached = tm.tmHeight > 0 ? (int)tm.tmHeight : 20;
    return cached;
}

// 深色主题下输入框的底色。拿不到 WM_CTLCOLOREDIT 时（顶层 EDIT 走的是 owner），
// 最差结果也只是「白底黑字的输入格」，永远可读，不会出现黑字黑底。
HBRUSH EditBgBrush() {
    if (g_dark) {
        if (!g_editBgDark) g_editBgDark = CreateSolidBrush(RGB(35, 35, 41));
        return g_editBgDark;
    }
    return (HBRUSH)GetStockObject(WHITE_BRUSH);
}

void EnsureBuffer() {
    const int w = kCardW + kShadow * 2;
    const int h = CardHeight() + kShadow * 2;
    if (g_memDC && g_bufW == w && g_bufH == h) return;
    if (g_memDC) { DeleteDC(g_memDC); g_memDC = nullptr; }
    if (g_dib) { DeleteObject(g_dib); g_dib = nullptr; }
    g_bits = nullptr;
    if (CreateDib(w, h, g_memDC, g_dib, g_bits)) {
        g_bufW = w;
        g_bufH = h;
    } else {
        g_bufW = g_bufH = 0;
    }
}

void Layout() {
    g_cardH = CardHeight();
    const int cx = kShadow, cy = kShadow;

    // 输入格（底槽）：卡片自己画
    const int fieldTop = cy + kPadTop + kTitleH + kAfterTitle;
    g_rcField = RECT{cx + kPadX, fieldTop, cx + kCardW - kPadX, fieldTop + kEditH};

    // EDIT 本体：只占一行高，垂直居中放进底槽
    int lh = FieldLineHeight() + 4;   // 上下各留 2px，抬一像素也不会被切掉
    if (lh > kEditH - 2) lh = kEditH - 2;
    const int editTop = fieldTop + (kEditH - lh) / 2;
    g_rcEdit = RECT{g_rcField.left + kFieldPadX, editTop, g_rcField.right - kFieldPadX,
                    editTop + lh};

    const int btnTop = cy + g_cardH - kPadBottom - kBtnH;
    const int okRight = cx + kCardW - kPadX;
    g_rcOk = RECT{okRight - kBtnW, btnTop, okRight, btnTop + kBtnH};
    g_rcCancel =
        RECT{g_rcOk.left - kBtnGap - kBtnW, btnTop, g_rcOk.left - kBtnGap, btnTop + kBtnH};
}

int HitTest(POINT c) {
    auto in = [&](const RECT& r) {
        return c.x >= r.left && c.x < r.right && c.y >= r.top && c.y < r.bottom;
    };
    if (in(g_rcOk)) return 1;
    if (in(g_rcCancel)) return 0;
    return -1;
}

// ---------------------------------------------------------------------------
// 绘制 + 提交
// ---------------------------------------------------------------------------
void Paint() {
    if (!g_memDC || !g_bits || g_bufW <= 0 || g_bufH <= 0) return;
    std::memset(g_bits, 0, (size_t)g_bufW * (size_t)g_bufH * 4);

    const bool light = !g_dark;
    const ARGB bg = light ? 0xFFFBFBFD : 0xFF2C2C30;
    const ARGB border = light ? 0xFFD9D9DE : 0xFF4A4A52;
    const ARGB fg = light ? 0xFF1B1B1F : 0xFFF0F0F4;
    // 「确定」是前进动作，用系统蓝；「取消」描边，不抢视线
    const ARGB accent = light ? 0xFF0067C0 : 0xFF0F6CBD;
    const ARGB accentHot = light ? 0xFF1A79CE : 0xFF2A86DA;

    {
        Bitmap target(g_bufW, g_bufH, g_bufW * 4, PixelFormat32bppPARGB,
                      reinterpret_cast<BYTE*>(g_bits));
        Graphics g(&target);
        g.SetSmoothingMode(SmoothingModeAntiAlias);
        g.SetPixelOffsetMode(PixelOffsetModeHighQuality);
        // 分层窗口 + alpha：不能用 ClearType，会出彩色杂边
        g.SetTextRenderingHint(TextRenderingHintAntiAlias);

        const RectF body((float)kShadow, (float)kShadow, (float)kCardW, (float)g_cardH);

        // 阴影：由外到内叠几层，越外越淡（和确认框同一个配方）
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
        Font fBtn(&fam, 13.0f, FontStyleRegular, UnitPixel);

        StringFormat leftAligned;
        leftAligned.SetLineAlignment(StringAlignmentCenter);
        leftAligned.SetFormatFlags(StringFormatFlagsNoWrap);
        leftAligned.SetTrimming(StringTrimmingEllipsisCharacter);

        // 标题
        {
            SolidBrush tb(fg);
            g.DrawString(g_title.c_str(), -1, &fTitle,
                         RectF((float)kShadow + kPadX, (float)kShadow + kPadTop,
                               (float)kCardW - kPadX * 2, (float)kTitleH),
                         &leftAligned, &tb);
        }

        // 输入格（底槽）：圆角 + 1px 描边，和整张卡片同一套配色。
        // 真正画字的是嵌在里面的 EDIT，它比这块窄一圈、只占一行高。
        {
            const RectF field((float)g_rcField.left, (float)g_rcField.top,
                              (float)(g_rcField.right - g_rcField.left),
                              (float)(g_rcField.bottom - g_rcField.top));
            GraphicsPath fp;
            AddRoundRect(fp, field, 8.0f);
            // 底色必须和 WM_CTLCOLOREDIT 里给 EDIT 的刷子一模一样，
            // 否则 EDIT 那一小块和底槽会有色差（拼成一块补丁）。
            SolidBrush fb(light ? 0xFFFFFFFF : 0xFF232329);
            g.FillPath(&fb, &fp);
            Pen fpen(light ? 0xFFD6D6DC : 0xFF3E3E46, 1.0f);
            g.DrawPath(&fpen, &fp);
        }

        auto drawButton = [&](const RECT& r, const wchar_t* label, bool primary, bool hot) {
            const RectF rf((float)r.left, (float)r.top, (float)(r.right - r.left),
                           (float)(r.bottom - r.top));
            GraphicsPath bp;
            AddRoundRect(bp, rf, 7.0f);
            StringFormat cf;
            cf.SetAlignment(StringAlignmentCenter);
            cf.SetLineAlignment(StringAlignmentCenter);
            cf.SetFormatFlags(StringFormatFlagsNoWrap);

            if (primary) {
                SolidBrush fb(hot ? accentHot : accent);
                g.FillPath(&fb, &bp);
                SolidBrush tb{Color(255, 255, 255, 255)};
                g.DrawString(label, -1, &fBtn, rf, &cf, &tb);
            } else {
                if (hot) {
                    SolidBrush hb(light ? Color(20, 0, 0, 0) : Color(26, 255, 255, 255));
                    g.FillPath(&hb, &bp);
                }
                Pen ep(light ? Color(255, 208, 208, 214) : Color(255, 84, 84, 92), 1.0f);
                g.DrawPath(&ep, &bp);
                SolidBrush tb{Color(fg)};
                g.DrawString(label, -1, &fBtn, rf, &cf, &tb);
            }
        };
        drawButton(g_rcCancel, L"取消", false, g_hover == 0);
        drawButton(g_rcOk, L"确定", true, g_hover == 1);

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
    g_hover = -1;
    g_press = -1;
    g_activated = false;
    auto cb = std::move(g_cb);
    g_cb = nullptr;
    auto done = std::move(g_onDone);
    g_onDone = nullptr;

    if (edit) DestroyWindow(edit);   // 先拆被拥有的那个，再拆卡片
    if (card) DestroyWindow(card);

    // 回调放在窗口销毁之后：里面会重新枚举图标、可能再弹确认框，
    // 不能在这些消息处理过程中把窗口拆掉。
    if (commit && !text.empty() && cb) cb(text);
    if (done) done();
}

void Cancel() { Finish(false); }

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
        // 焦点可能刚跑到卡片上的按钮（同一线程），也可能跑到别的程序。
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
        // 点到卡片自己的按钮时，前台窗口还是这张卡片 —— 不能当「跑了」
        if (g_card && g_activated) {
            HWND fg = GetForegroundWindow();
            if (fg != g_card && fg != g_edit) Finish(false);
        }
        return 0;

    case WM_ACTIVATE:
        if (LOWORD(w) != WA_INACTIVE) g_activated = true;
        return 0;

    case WM_MOUSEACTIVATE:
        return MA_ACTIVATE;

    case WM_MOUSEMOVE: {
        POINT pt{GET_X_LPARAM(l), GET_Y_LPARAM(l)};
        const int hot = HitTest(pt);
        if (hot != g_hover) {
            g_hover = hot;
            Paint();
            Present();
        }
        TRACKMOUSEEVENT tme{sizeof(tme), TME_LEAVE, h, 0};
        TrackMouseEvent(&tme);
        return 0;
    }

    case WM_MOUSELEAVE:
        if (g_hover != -1) {
            g_hover = -1;
            Paint();
            Present();
        }
        return 0;

    case WM_SETCURSOR: {
        if (LOWORD(l) == HTCLIENT) {
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
        POINT pt{GET_X_LPARAM(l), GET_Y_LPARAM(l)};
        g_press = HitTest(pt);
        if (g_press >= 0) {
            SetCapture(h);
        } else if (g_edit) {
            // 点在卡片空白处：这一下已经把焦点从输入框抢走了，还给输入框，
            // 否则点一下卡片之后就没法继续打字了。
            SetFocus(g_edit);
        }
        return 0;
    }

    case WM_LBUTTONUP: {
        POINT pt{GET_X_LPARAM(l), GET_Y_LPARAM(l)};
        if (GetCapture() == h) ReleaseCapture();
        const int was = g_press;
        g_press = -1;
        if (was >= 0 && was == HitTest(pt)) {
            if (was == 1) Finish(true);
            else Finish(false);
        }
        return 0;
    }

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
    wc.lpszClassName = kCardClass;
    RegisterClassExW(&wc);
}

}  // namespace

// ---------------------------------------------------------------------------
// 对外接口
// ---------------------------------------------------------------------------
bool IsOpen() { return g_card != nullptr; }

void Close() { Cancel(); }

void Show(HWND anchor, const std::wstring& title, const std::wstring& currentName,
          std::function<void(const std::wstring&)> onCommit, Sel sel,
          std::function<void()> onDone) {
    Close();   // 之前还开着一个就先按取消收掉
    EnsureGdiplus();
    EnsureCardClass();
    EnsureBuffer();
    if (!g_memDC) {
        // 建不出位图（极端内存压力）：别把调用方的收尾回调丢了
        g_cb = nullptr;
        g_onDone = nullptr;
        if (onDone) onDone();
        return;
    }

    g_cb = std::move(onCommit);
    g_onDone = std::move(onDone);
    g_dark = !LightTheme();
    g_activated = false;
    g_hover = -1;
    g_press = -1;
    g_title = title;
    Layout();

    // 摆位：居中于 anchor（盒子）；没有 anchor 就居中于鼠标所在屏幕
    RECT a{};
    if (!anchor || !GetWindowRect(anchor, &a))
        a = RECT{0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN)};

    int x = ((int)a.left + (int)a.right - g_bufW) / 2;
    int y = ((int)a.top + (int)a.bottom - g_bufH) / 2;

    // 夹进锚点所在显示器的工作区：盒子可能一半在屏幕外，卡片不能跟着出去
    MONITORINFO mi{};
    mi.cbSize = sizeof(mi);
    HMONITOR mon = MonitorFromPoint(
        POINT{((int)a.left + (int)a.right) / 2, ((int)a.top + (int)a.bottom) / 2},
        MONITOR_DEFAULTTONEAREST);
    RECT lim{0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN)};
    if (mon && GetMonitorInfoW(mon, &mi)) lim = mi.rcWork;
    const int limL = (int)lim.left, limT = (int)lim.top;
    const int limR = (int)lim.right, limB = (int)lim.bottom;
    if (x + g_bufW > limR) x = limR - g_bufW;
    if (y + g_bufH > limB) y = limB - g_bufH;
    if (x < limL) x = limL;
    if (y < limT) y = limT;

    g_card = CreateWindowExW(WS_EX_LAYERED | WS_EX_TOOLWINDOW | WS_EX_TOPMOST, kCardClass, L"",
                             WS_POPUP, x, y, g_bufW, g_bufH, nullptr, nullptr,
                             GetModuleHandleW(nullptr), nullptr);
    if (!g_card) {
        g_cb = nullptr;
        g_onDone = nullptr;
        if (onDone) onDone();
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
    SendMessageW(g_edit, WM_SETFONT, (WPARAM)DialogFont(), TRUE);
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

    // 输入框在卡片显示之后再显一次：它是「卡片拥有」的窗口，
    // owner 从隐藏变可见时它不一定跟着显示，漏这一步会「弹窗出来了但没法打字」。
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

}  // namespace RenameDialog

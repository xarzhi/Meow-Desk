#include "todo_dialog.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <utility>

#include <windowsx.h>
#include <commctrl.h>   // SetWindowSubclass / DefSubclassProc

// GDI+ 头文件顺序：Image / PropertyItem 需要 IStream / PROPID
#include <objidl.h>
#include <propidl.h>
#include <gdiplus.h>

#include "icon_render.h"

using namespace Gdiplus;

namespace TodoDialog {
namespace {

const wchar_t* kCardClass = L"MeowDeskTodoCard";

// ---- 几何（和 rename_dialog / confirm_dialog 同一套口径）----
const int kShadow = 14;      // 阴影留白：客户区比卡片每边大这么多
const int kRound = 10;
const int kCardW = 420;      // 比改名弹窗宽一点：详情正文要放得下一整行话
const int kPadX = 24;
const int kPadTop = 22;
const int kTitleH = 22;
const int kAfterTitle = 16;
const int kEditH = 32;       // 新建待办的输入底槽
const int kFieldPadX = 10;
const int kAfterEdit = 20;
const int kBtnW = 92;
const int kBtnH = 32;
const int kBtnGap = 10;
const int kPadBottom = 22;
const int kDetailLineH = 21;
// 详情正文最多 6 行：再多卡片会长到屏幕外（待办文字本身限 200 字）
const int kDetailMaxH = kDetailLineH * 6;

// 「焦点跑掉了没有」的延迟检查（见 CardProc 的 kMsgCheckFocus）。
// 和 rename_dialog 的 WM_APP+12 错开，避免万一两条消息串到同一个窗口上。
const UINT kMsgCheckFocus = WM_APP + 13;

enum class Mode { Input, Detail };

HWND g_card = nullptr;
HWND g_edit = nullptr;   // 只有 Input 模式有
Mode g_mode = Mode::Input;
std::wstring g_title;
std::wstring g_text;     // Detail 模式的正文
bool g_done = false;     // Detail 模式：这条待办当前是不是已完成
std::function<void(const std::wstring&)> g_onOk;
std::function<void()> g_onToggle;
std::function<void()> g_onDone;

bool g_activated = false;
bool g_dark = false;
int g_hover = -1;        // 0 = 次要按钮，1 = 主按钮，-1 = 空白
int g_press = -1;

int g_cardH = 0;
int g_textH = 0;         // Detail 模式正文块的高度（Layout 前算好）
RECT g_rcField{};
RECT g_rcEdit{};
RECT g_rcOk{};
RECT g_rcCancel{};

HDC g_memDC = nullptr;
HBITMAP g_dib = nullptr;
void* g_bits = nullptr;
int g_bufW = 0;
int g_bufH = 0;

HBRUSH g_editBgDark = nullptr;

// ---------------------------------------------------------------------------
// 配色 / 字体 / 画法
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

// 输入框窗口的高度用字体行高推，不能写死像素：换系统字号 / 高 DPI 会露半个字。
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

HBRUSH EditBgBrush() {
    if (g_dark) {
        if (!g_editBgDark) g_editBgDark = CreateSolidBrush(RGB(35, 35, 41));
        return g_editBgDark;
    }
    return (HBRUSH)GetStockObject(WHITE_BRUSH);
}

// 详情正文「按卡片宽度换行后有多高」。卡片高度要在建缓冲之前定下来，所以必须先量。
int MeasureDetailHeight(const std::wstring& s) {
    const int textW = kCardW - kPadX * 2;
    int h = kDetailLineH;
    HDC dc = GetDC(nullptr);
    if (dc) {
        {
            Graphics g(dc);
            g.SetTextRenderingHint(TextRenderingHintAntiAlias);
            FontFamily fam(L"Microsoft YaHei UI");
            Font f(&fam, 14.0f, FontStyleRegular, UnitPixel);
            StringFormat fmt(StringFormatFlagsLineLimit);
            RectF bound;
            g.MeasureString(s.c_str(), -1, &f, RectF(0.0f, 0.0f, (float)textW, 4000.0f), &fmt,
                            &bound);
            const int mh = (int)std::ceil(bound.Height);
            if (mh > h) h = mh;
        }
        ReleaseDC(nullptr, dc);
    }
    return std::clamp(h, kDetailLineH, kDetailMaxH);
}

int CardHeight() {
    if (g_mode == Mode::Detail)
        return kPadTop + kTitleH + kAfterTitle + g_textH + kAfterEdit + kBtnH + kPadBottom;
    return kPadTop + kTitleH + kAfterTitle + kEditH + kAfterEdit + kBtnH + kPadBottom;
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

    if (g_mode == Mode::Input) {
        const int fieldTop = cy + kPadTop + kTitleH + kAfterTitle;
        g_rcField = RECT{cx + kPadX, fieldTop, cx + kCardW - kPadX, fieldTop + kEditH};

        // EDIT 本体只占一行高，垂直居中嵌进底槽 —— 单行 EDIT 是从客户区顶部排字的，
        // 直接给 32px 高会贴着上半截，看着没居中（rename_dialog 踩过同一个坑）。
        int lh = FieldLineHeight() + 4;
        if (lh > kEditH - 2) lh = kEditH - 2;
        const int editTop = fieldTop + (kEditH - lh) / 2;
        g_rcEdit = RECT{g_rcField.left + kFieldPadX, editTop, g_rcField.right - kFieldPadX,
                        editTop + lh};
    } else {
        g_rcField = RECT{};
        g_rcEdit = RECT{};
    }

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

void Paint() {
    if (!g_memDC || !g_bits || g_bufW <= 0 || g_bufH <= 0) return;
    std::memset(g_bits, 0, (size_t)g_bufW * (size_t)g_bufH * 4);

    const bool light = !g_dark;
    const ARGB bg = light ? 0xFFFBFBFD : 0xFF2C2C30;
    const ARGB border = light ? 0xFFD9D9DE : 0xFF4A4A52;
    const ARGB fg = light ? 0xFF1B1B1F : 0xFFF0F0F4;
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

        // 阴影：由外到内叠几层，越外越淡（和确认框 / 改名弹窗同一个配方）
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
        Font fBody(&fam, 14.0f, FontStyleRegular, UnitPixel);

        StringFormat oneLine;
        oneLine.SetLineAlignment(StringAlignmentCenter);
        oneLine.SetFormatFlags(StringFormatFlagsNoWrap);
        oneLine.SetTrimming(StringTrimmingEllipsisCharacter);

        // 标题
        {
            SolidBrush tb(fg);
            g.DrawString(g_title.c_str(), -1, &fTitle,
                         RectF((float)kShadow + kPadX, (float)kShadow + kPadTop,
                               (float)kCardW - kPadX * 2, (float)kTitleH),
                         &oneLine, &tb);

            // 详情模式：标题右边一个小徽标标状态 —— 弹窗里除了正文，用户最想知道
            // 就是「这条到底做完了没有」。不占额外高度，直接挂在标题这一行右端。
            if (g_mode == Mode::Detail) {
                Font fBadge(&fam, 12.0f, FontStyleRegular, UnitPixel);
                const float bw = 56.0f, bh = 20.0f;
                const float bx = (float)(kShadow + kCardW - kPadX) - bw;
                const float by = (float)kShadow + kPadTop + ((float)kTitleH - bh) * 0.5f;
                const RectF br(bx, by, bw, bh);
                GraphicsPath bp;
                AddRoundRect(bp, br, 10.0f);
                if (g_done) {
                    SolidBrush sb(light ? Color(20, 0, 0, 0) : Color(26, 255, 255, 255));
                    g.FillPath(&sb, &bp);
                } else {
                    SolidBrush sb(light ? Color(26, 0, 103, 192) : Color(38, 96, 205, 255));
                    g.FillPath(&sb, &bp);
                }
                StringFormat cf;
                cf.SetAlignment(StringAlignmentCenter);
                cf.SetLineAlignment(StringAlignmentCenter);
                cf.SetFormatFlags(StringFormatFlagsNoWrap);
                SolidBrush stb{Color(g_done ? fg : accent)};
                g.DrawString(g_done ? L"\u5df2\u5b8c\u6210" : L"\u672a\u5b8c\u6210", -1,
                             &fBadge, br, &cf, &stb);   // 已完成 / 未完成
            }
        }

        if (g_mode == Mode::Input) {
            // 输入底槽：圆角 + 1px 描边；真正画字的是嵌在里面的 EDIT
            const RectF field((float)g_rcField.left, (float)g_rcField.top,
                              (float)(g_rcField.right - g_rcField.left),
                              (float)(g_rcField.bottom - g_rcField.top));
            GraphicsPath fp;
            AddRoundRect(fp, field, 8.0f);
            // 底色必须和 WM_CTLCOLOREDIT 给 EDIT 的刷子一模一样，否则会有色差补丁
            SolidBrush fb(light ? 0xFFFFFFFF : 0xFF232329);
            g.FillPath(&fb, &fp);
            Pen fpen(light ? 0xFFD6D6DC : 0xFF3E3E46, 1.0f);
            g.DrawPath(&fpen, &fp);
        } else {
            // 详情正文：自动换行，最多 6 行，超出打省略号
            const int textTop = kShadow + kPadTop + kTitleH + kAfterTitle;
            const RectF tr((float)kShadow + kPadX, (float)textTop,
                           (float)kCardW - kPadX * 2, (float)g_textH);
            StringFormat wrap;
            wrap.SetFormatFlags(StringFormatFlagsLineLimit);
            wrap.SetTrimming(StringTrimmingEllipsisCharacter);
            wrap.SetAlignment(StringAlignmentNear);
            // 注意花括号初始化：`SolidBrush tb(Color(fg));` 会被当成**函数声明**
            // （most vexing parse），然后 &tb 就成了函数指针，编译期报「没有重载」。
            SolidBrush tb{Color(fg)};
            g.DrawString(g_text.c_str(), -1, &fBody, tr, &wrap, &tb);
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

        if (g_mode == Mode::Detail) {
            drawButton(g_rcCancel, L"\u5173\u95ed", false, g_hover == 0);   // 关闭
            // 已完成的那条，主按钮变成反操作 —— 不可能让用户在详情里「再完成一次」
            drawButton(g_rcOk, g_done ? L"\u6062\u590d\u672a\u5b8c\u6210" : L"\u5b8c\u6210",
                       true, g_hover == 1);
        } else {
            drawButton(g_rcCancel, L"\u53d6\u6d88", false, g_hover == 0);   // 取消
            drawButton(g_rcOk, L"\u786e\u5b9a", true, g_hover == 1);        // 确定
        }

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

// 拆窗 + 把回调取出来。两条收尾路径（确定 / 取消 / 点主按钮）都走这里，
// 先清状态再 DestroyWindow —— 销毁过程中还会收到 WM_KILLFOCUS / WM_ACTIVATE，
// 不清就会递归回来。
void Teardown(std::function<void(const std::wstring&)>& ok, std::function<void()>& toggle,
              std::function<void()>& done, std::wstring& text) {
    HWND card = g_card;
    HWND edit = g_edit;
    g_card = nullptr;
    g_edit = nullptr;
    g_hover = -1;
    g_press = -1;
    g_activated = false;

    ok = std::move(g_onOk);
    toggle = std::move(g_onToggle);
    done = std::move(g_onDone);
    g_onOk = nullptr;
    g_onToggle = nullptr;
    g_onDone = nullptr;
    text = g_text;

    if (edit) DestroyWindow(edit);   // 先拆被拥有的那个，再拆卡片
    if (card) DestroyWindow(card);
}

// 新建待办：commit = true 时把输入框里的文字交出去（空字符串 = 当没写）
void FinishInput(bool commit) {
    if (!g_card) return;
    std::wstring input;
    if (commit && g_edit) {
        // WM_GETTEXT 而不是 GetWindowTextW：文本也可能是自动化脚本 WM_SETTEXT 写进来的
        wchar_t buf[512]{};
        SendMessageW(g_edit, WM_GETTEXT, (WPARAM)511, (LPARAM)buf);
        input = buf;
        size_t a = 0, b = input.size();
        while (a < b && (input[a] == L' ' || input[a] == L'\t' || input[a] == L'\r' ||
                         input[a] == L'\n'))
            ++a;
        while (b > a && (input[b - 1] == L' ' || input[b - 1] == L'\t' ||
                         input[b - 1] == L'\r' || input[b - 1] == L'\n'))
            --b;
        input = input.substr(a, b - a);
    }

    std::function<void(const std::wstring&)> ok;
    std::function<void()> toggle, done;
    std::wstring ignored;
    Teardown(ok, toggle, done, ignored);

    // 回调放在窗口销毁之后：里面会存配置、重画盒子
    if (commit && !input.empty() && ok) ok(input);
    if (done) done();
}

// 详情：toggle = true 表示点了主按钮（完成 / 恢复）
void FinishDetail(bool toggleNow) {
    if (!g_card) return;
    std::function<void(const std::wstring&)> ok;
    std::function<void()> toggle, done;
    std::wstring ignored;
    Teardown(ok, toggle, done, ignored);
    if (toggleNow && toggle) toggle();
    if (done) done();
}

void CloseCard() {
    if (g_mode == Mode::Detail)
        FinishDetail(false);
    else
        FinishInput(false);
}

// ---------------------------------------------------------------------------
// 消息处理
// ---------------------------------------------------------------------------
LRESULT CALLBACK EditProc(HWND h, UINT m, WPARAM w, LPARAM l, UINT_PTR, DWORD_PTR) {
    switch (m) {
    case WM_KEYDOWN:
        if (w == VK_RETURN) {
            FinishInput(true);
            return 0;
        }
        if (w == VK_ESCAPE) {
            FinishInput(false);
            return 0;
        }
        break;
    case WM_CHAR:
        // 单行 EDIT 收到回车 / Esc 会「叮」一声，已经自己处理掉了就别再响
        if (w == L'\r' || w == L'\n' || w == 27) return 0;
        break;
    case WM_GETDLGCODE:
        return DLGC_WANTALLKEYS;
    case WM_SETFOCUS:
        g_activated = true;   // 真正拿到键盘焦点才算立住
        break;
    case WM_KILLFOCUS:
        // 焦点可能刚跑到卡片按钮（同线程）也可能跑到别的程序，等这轮消息走完再看
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
        // 点卡片自己的按钮时前台还是这张卡片，不能当「跑了」
        if (g_card && g_activated) {
            HWND fg = GetForegroundWindow();
            if (fg != g_card && fg != g_edit) CloseCard();
        }
        return 0;

    case WM_ACTIVATE:
        if (LOWORD(w) != WA_INACTIVE) g_activated = true;
        return 0;

    case WM_MOUSEACTIVATE:
        return MA_ACTIVATE;

    case WM_KEYDOWN:
        // 详情模式没有输入框，键盘得卡片自己接（Esc 关掉、回车 = 主按钮）。
        // 新建模式走 EDIT 的 EditProc，这里别把按键吃掉（否则 Tab / 方向键全哑）。
        if (g_mode == Mode::Detail) {
            if (w == VK_ESCAPE) {
                FinishDetail(false);
                return 0;
            }
            if (w == VK_RETURN) {
                FinishDetail(true);
                return 0;
            }
            return 0;
        }
        break;

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
            // 点卡片空白处会把焦点从输入框抢走，还给输入框，否则之后打不了字
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
            if (g_mode == Mode::Detail)
                FinishDetail(was == 1);
            else
                FinishInput(was == 1);
        }
        return 0;
    }

    case WM_CTLCOLOREDIT: {
        HDC dc = (HDC)w;
        SetTextColor(dc, g_dark ? RGB(240, 240, 244) : RGB(27, 27, 31));
        SetBkColor(dc, g_dark ? RGB(35, 35, 41) : RGB(255, 255, 255));
        return (LRESULT)EditBgBrush();
    }

    case WM_ERASEBKGND:
        return 1;

    case WM_CLOSE:
        CloseCard();
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

// 卡片摆放：**居中于屏幕**（用户明确要求「在屏幕中央」），屏幕取 owner 所在那块
// （待办盒可能在副屏上），没有 owner 就取鼠标所在那块。居中于盒子是不对的 ——
// 盒子本身就在屏幕角落，卡片贴着角落出来很别扭。
void PlaceAndShow(HWND owner, bool wantInputFocus) {
    POINT ref{};
    if (owner) {
        RECT a{};
        if (GetWindowRect(owner, &a))
            ref = POINT{((int)a.left + (int)a.right) / 2, ((int)a.top + (int)a.bottom) / 2};
        else
            GetCursorPos(&ref);
    } else {
        GetCursorPos(&ref);
    }

    HMONITOR mon = MonitorFromPoint(ref, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi{};
    mi.cbSize = sizeof(mi);
    RECT lim{0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN)};
    if (mon && GetMonitorInfoW(mon, &mi)) lim = mi.rcWork;

    int x = ((int)lim.left + (int)lim.right - g_bufW) / 2;
    int y = ((int)lim.top + (int)lim.bottom - g_bufH) / 2;
    // 工作区比卡片还小（极端缩放）时至少别把卡片顶到屏幕外
    if (x < (int)lim.left) x = (int)lim.left;
    if (y < (int)lim.top) y = (int)lim.top;

    g_card = CreateWindowExW(WS_EX_LAYERED | WS_EX_TOOLWINDOW | WS_EX_TOPMOST, kCardClass, L"",
                             WS_POPUP, x, y, g_bufW, g_bufH, nullptr, nullptr,
                             GetModuleHandleW(nullptr), nullptr);
    if (!g_card) return;

    if (wantInputFocus) {
        // 输入框是「卡片拥有的顶层弹出窗口」：分层窗口不能放子控件（不会被合成），
        // owned 窗口永远压在 owner 上面，而且键盘焦点 / 光标 / 输入法全是系统原生的。
        g_edit = CreateWindowExW(WS_EX_TOOLWINDOW, L"EDIT", L"",
                                 WS_POPUP | WS_VISIBLE | ES_AUTOHSCROLL, x + (int)g_rcEdit.left,
                                 y + (int)g_rcEdit.top, (int)(g_rcEdit.right - g_rcEdit.left),
                                 (int)(g_rcEdit.bottom - g_rcEdit.top), g_card, nullptr,
                                 GetModuleHandleW(nullptr), nullptr);
        if (g_edit) {
            SendMessageW(g_edit, WM_SETFONT, (WPARAM)DialogFont(), TRUE);
            SendMessageW(g_edit, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN,
                         MAKELPARAM(0, 0));
            SendMessageW(g_edit, EM_SETLIMITTEXT, 200, 0);
            SetWindowSubclass(g_edit, EditProc, 1, 0);
        }
    }

    SetWindowPos(g_card, HWND_TOPMOST, x, y, g_bufW, g_bufH, SWP_SHOWWINDOW | SWP_NOACTIVATE);
    Paint();
    Present();

    if (g_edit) {
        // owner 从隐藏变可见时 owned 窗口不一定跟着显示，漏这一步会「弹窗出来但打不了字」
        ShowWindow(g_edit, SW_SHOWNA);
        SetWindowPos(g_edit, HWND_TOPMOST, x + (int)g_rcEdit.left, y + (int)g_rcEdit.top,
                     (int)(g_rcEdit.right - g_rcEdit.left),
                     (int)(g_rcEdit.bottom - g_rcEdit.top), SWP_SHOWWINDOW | SWP_NOACTIVATE);
    }

    // 主窗口都是 WS_EX_NOACTIVATE，进程得做一次「真正激活」这个弹窗，
    // 输入框才拿得到键盘焦点（右键菜单刚关掉，前台权利还在我们手上）。
    HWND target = g_edit ? g_edit : g_card;
    SetForegroundWindow(target);
    SetActiveWindow(target);
    SetFocus(target);
}

void ResetState(Mode mode, const std::wstring& title) {
    g_mode = mode;
    g_title = title;
    g_dark = !LightTheme();
    g_activated = false;
    g_hover = -1;
    g_press = -1;
    g_text.clear();
    g_done = false;
    g_textH = 0;
}

}  // namespace

// ---------------------------------------------------------------------------
// 对外接口
// ---------------------------------------------------------------------------
bool IsOpen() { return g_card != nullptr; }

void Close() { CloseCard(); }

void ShowInput(HWND owner, const std::wstring& initial,
               std::function<void(const std::wstring&)> onOk, std::function<void()> onDone) {
    Close();   // 已经开着一个就先按取消收掉
    EnsureGdiplus();
    EnsureCardClass();
    ResetState(Mode::Input, L"\u65b0\u5efa\u5f85\u529e");   // 新建待办
    EnsureBuffer();
    if (!g_memDC) {
        // 建不出位图（极端内存压力）：别把调用方的收尾回调丢了
        if (onDone) onDone();
        return;
    }
    g_onOk = onOk;       // 拷一份，别 move：下面几条失败路径还要用原来的 onDone
    g_onDone = onDone;
    Layout();
    PlaceAndShow(owner, /*wantInputFocus=*/true);
    if (!g_card) {
        g_onOk = nullptr;
        g_onDone = nullptr;
        if (onDone) onDone();
        return;
    }
    if (!g_edit) {
        // 输入框没建出来（极端情况）：这个模式没有意义，直接收掉并告诉调用方
        FinishInput(false);
        return;
    }
    SetWindowTextW(g_edit, initial.c_str());
    SendMessageW(g_edit, EM_SETSEL, 0, -1);
    SendMessageW(g_edit, EM_SCROLLCARET, 0, 0);
    Paint();
    Present();
}

void ShowDetail(HWND owner, const std::wstring& text, bool done,
                std::function<void()> onToggleDone, std::function<void()> onDone) {
    Close();
    EnsureGdiplus();
    EnsureCardClass();
    ResetState(Mode::Detail, L"\u5f85\u529e\u8be6\u60c5");   // 待办详情
    g_text = text;
    g_done = done;
    g_textH = MeasureDetailHeight(g_text);   // 卡片高度要先知道正文多高
    EnsureBuffer();
    if (!g_memDC) {
        if (onDone) onDone();
        return;
    }
    g_onToggle = onToggleDone;
    g_onDone = onDone;
    Layout();
    PlaceAndShow(owner, /*wantInputFocus=*/false);
    if (!g_card) {
        g_onToggle = nullptr;
        g_onDone = nullptr;
        if (onDone) onDone();
    }
}

}  // namespace TodoDialog

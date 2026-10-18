#include "search_window.h"

#include <commctrl.h>
#include <dwmapi.h>
#include <imm.h>
#include <objidl.h>
#include <propidl.h>
#include <windowsx.h>

#include <algorithm>
#include <cmath>
#include <cwctype>

#include <gdiplus.h>

#include "icon_render.h"

#pragma comment(lib, "gdiplus.lib")
#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "comctl32.lib")

using namespace Gdiplus;

namespace {

const wchar_t* kClass = L"MeowDeskSearch";
const int kWidth = 680;
const int kRowH = 46;
const int kMaxRows = 8;
const int kPad = 16;
const int kSearchH = 46;
const int kListTop = kPad + kSearchH + 10;
const int kFooterH = 30;

const int kEditId = 1001;

enum : int { ID_NONE = -1 };

bool AppsUseLightTheme() {
    HKEY k = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER,
                      L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize", 0,
                      KEY_READ, &k) != ERROR_SUCCESS)
        return true;
    DWORD v = 1, sz = sizeof(v);
    RegQueryValueExW(k, L"AppsUseLightTheme", nullptr, nullptr, (LPBYTE)&v, &sz);
    RegCloseKey(k);
    return v != 0;
}

struct Theme {
    COLORREF bg;
    COLORREF search;
    COLORREF border;
    COLORREF text;
    COLORREF sub;
    COLORREF accent;
    COLORREF selBg;
    COLORREF hoverBg;
};

Theme LightTheme() {
    return {RGB(249, 249, 249), RGB(255, 255, 255), RGB(228, 228, 228), RGB(26, 26, 26),
            RGB(120, 120, 120), RGB(0, 103, 192), RGB(230, 240, 252), RGB(238, 238, 238)};
}

Theme DarkTheme() {
    return {RGB(43, 43, 43), RGB(56, 56, 56), RGB(64, 64, 64), RGB(245, 245, 245),
            RGB(160, 160, 160), RGB(76, 194, 255), RGB(38, 61, 84), RGB(56, 56, 56)};
}

Color C(COLORREF c) { return Color(255, GetRValue(c), GetGValue(c), GetBValue(c)); }
Color CA(COLORREF c, int a) {
    return Color((BYTE)a, GetRValue(c), GetGValue(c), GetBValue(c));
}

void AddRound(GraphicsPath& p, const RectF& r, float rad) {
    const float d = std::min({rad * 2.0f, r.Width, r.Height});
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

std::wstring Lower(std::wstring s) {
    for (auto& c : s) c = (wchar_t)towlower(c);
    return s;
}

// 汉字 → 拼音首字母。GB2312 一级汉字（16~55 区）本身就是按拼音排序的，
// 用区位码落在哪一段就能反推首字母，不需要几万字的拼音表。
// 边界值 = 各组第一个字的区位码（啊 芭 擦 搭 蛾 发 噶 哈 击 喀 垃 妈 拿 哦 啪 期
// 然 撒 塌 挖 昔 压 匝），末尾 5587 = 一级汉字「做」之后。
wchar_t PinyinInitial(wchar_t ch) {
    if (ch < 0x80) return (wchar_t)towlower(ch);
    char gbk[4]{};
    if (WideCharToMultiByte(936, 0, &ch, 1, gbk, 3, nullptr, nullptr) != 2) return 0;
    const unsigned char h = (unsigned char)gbk[0], l = (unsigned char)gbk[1];
    if (h < 0xA1 || l < 0xA1) return 0;
    const int code = (h - 0xA0) * 100 + (l - 0xA0);  // 区位码
    if (code < 1601 || code >= 5587) return 0;       // 一级汉字之外（生僻字/二级字）不参与
    static const int kBound[] = {1601, 1637, 1833, 2078, 2274, 2302, 2433, 2594, 2787, 3106,
                                 3212, 3472, 3635, 3722, 3730, 3858, 4027, 4086, 4390, 4558,
                                 4684, 4925, 5249, 5587};
    static const wchar_t kPy[] = L"abcdefghjklmnopqrstwxyz";
    for (int i = 0; i < 23; ++i)
        if (code >= kBound[i] && code < kBound[i + 1]) return kPy[i];
    return 0;
}

// 「网易云音乐」→ "wyyyy"；非汉字原样小写。用来支持拼音首字母搜索。
std::wstring PinyinInitials(const std::wstring& name) {
    std::wstring out;
    out.reserve(name.size());
    for (wchar_t c : name) {
        const wchar_t p = PinyinInitial(c);
        if (p) out.push_back(p);
    }
    return out;
}

// 去掉分隔符再比。搜狗/微软拼音在组合串里会插音节分隔符，比如输入 wyy 时
// 的 GCS_COMPSTR 是 "w'y'y"，不剥掉就永远匹配不上 "wyyyl"。
bool IsSeparator(wchar_t c) {
    return c == L'\'' || c == L' ' || c == L'-' || c == L'_' || c == L'.' || c == L'\t';
}

std::wstring StripSeparators(const std::wstring& s) {
    std::wstring out;
    out.reserve(s.size());
    for (wchar_t c : s)
        if (!IsSeparator(c)) out.push_back(c);
    return out;
}

}  // namespace

LRESULT CALLBACK SearchWindow::WndProcStatic(HWND h, UINT m, WPARAM w, LPARAM l) {
    SearchWindow* self = reinterpret_cast<SearchWindow*>(GetWindowLongPtrW(h, GWLP_USERDATA));
    if (m == WM_NCCREATE) {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(l);
        self = reinterpret_cast<SearchWindow*>(cs->lpCreateParams);
        SetWindowLongPtrW(h, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    }
    if (self) return self->WndProc(h, m, w, l);
    return DefWindowProcW(h, m, w, l);
}

// EDIT 子类化：把光标键 / 回车 / Esc 交给面板处理（EDIT 自己会吞掉方向键）
LRESULT CALLBACK SearchWindow::EditProc(HWND h, UINT m, WPARAM w, LPARAM l, UINT_PTR,
                                        DWORD_PTR ref) {
    auto* self = reinterpret_cast<SearchWindow*>(ref);
    if (!self) return DefSubclassProc(h, m, w, l);

    // IME 消息只发给持有输入焦点的窗口，也就是这个 EDIT，所以组合串要在这里接
    switch (m) {
    case WM_IME_STARTCOMPOSITION:
        self->imeComp_.clear();
        break;
    case WM_IME_COMPOSITION:
        if (l & (GCS_COMPSTR | GCS_RESULTSTR)) {
            // 先让 EDIT 自己把上屏结果写进文本，再读组合串
            const LRESULT r = DefSubclassProc(h, m, w, l);
            self->SyncImeComposition(h);
            return r;
        }
        break;
    case WM_IME_ENDCOMPOSITION:
        self->imeComp_.clear();
        break;
    default:
        break;
    }

    if (m == WM_KEYDOWN) {
        switch (w) {
        case VK_ESCAPE:
            self->Hide();
            return 0;
        case VK_RETURN:
            self->Activate();
            return 0;
        case VK_UP:
            self->MoveSel(-1);
            return 0;
        case VK_DOWN:
            self->MoveSel(1);
            return 0;
        case VK_NEXT:
            self->MoveSel(kMaxRows);
            return 0;
        case VK_PRIOR:
            self->MoveSel(-kMaxRows);
            return 0;
        default:
            break;
        }
    }
    return DefSubclassProc(h, m, w, l);
}

// 读 IME 的未上屏组合串（拼音），并立刻重新过滤
void SearchWindow::SyncImeComposition(HWND edit) {
    HIMC himc = ImmGetContext(edit);
    if (!himc) {
        imeComp_.clear();
    } else {
        const LONG bytes = ImmGetCompositionStringW(himc, GCS_COMPSTR, nullptr, 0);
        if (bytes > 0) {
            std::wstring s((size_t)bytes / sizeof(wchar_t), L'\0');
            ImmGetCompositionStringW(himc, GCS_COMPSTR, &s[0], bytes);
            imeComp_ = s;
        } else {
            imeComp_.clear();
        }
        ImmReleaseContext(edit, himc);
    }
    Filter();
    Repaint();
}

// 组合态下 IME 会把 Esc 吞掉（用来取消组字），面板就关不掉了。
// 面板可见期间挂一个低级键盘钩子，把 Esc 抢回来并直接关面板。
static SearchWindow* g_kbTarget = nullptr;

LRESULT CALLBACK SearchWindow::LowLevelKb(int code, WPARAM wp, LPARAM lp) {
    if (code == HC_ACTION && (wp == WM_KEYDOWN || wp == WM_SYSKEYDOWN)) {
        auto* kb = reinterpret_cast<KBDLLHOOKSTRUCT*>(lp);
        if (kb->vkCode == VK_ESCAPE && g_kbTarget && g_kbTarget->visible()) {
            g_kbTarget->Hide();
            return 1;  // 吞掉，别让 IME 也处理一遍
        }
    }
    return CallNextHookEx(nullptr, code, wp, lp);
}

void SearchWindow::HookKeyboard() {
    if (kbHook_) return;
    g_kbTarget = this;
    kbHook_ = SetWindowsHookExW(WH_KEYBOARD_LL, &SearchWindow::LowLevelKb,
                                GetModuleHandleW(nullptr), 0);
    if (!kbHook_) g_kbTarget = nullptr;
}

void SearchWindow::UnhookKeyboard() {
    if (kbHook_) {
        UnhookWindowsHookEx(kbHook_);
        kbHook_ = nullptr;
    }
    if (g_kbTarget == this) g_kbTarget = nullptr;
}

int SearchWindow::RowsVisible() const {
    return std::min<int>((int)hits_.size(), kMaxRows);
}

RECT SearchWindow::RowRect(int i) const {
    RECT r{};
    r.left = kPad;
    r.right = kWidth - kPad;
    r.top = kListTop + i * kRowH;
    r.bottom = r.top + kRowH - 2;
    return r;
}

void SearchWindow::Layout() {
    if (!hwnd_) return;
    RECT cr{};
    cr.right = kWidth;
    cr.bottom = kListTop + kMaxRows * kRowH + kFooterH;
    const int h = cr.bottom;

    SetWindowPos(hwnd_, nullptr, 0, 0, kWidth, h, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);

    if (!font_) {
        LOGFONTW lf{};
        lf.lfHeight = -18;
        lf.lfWeight = FW_NORMAL;
        lf.lfQuality = CLEARTYPE_QUALITY;
        wcscpy_s(lf.lfFaceName, L"Microsoft YaHei UI");
        font_ = CreateFontIndirectW(&lf);
    }
    if (!edit_) {
        edit_ = CreateWindowExW(0, L"EDIT", L"",
                                WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL | ES_LEFT, kPad + 40,
                                kPad + 8, kWidth - kPad * 2 - 52, kSearchH - 16, hwnd_,
                                (HMENU)(INT_PTR)kEditId, GetModuleHandleW(nullptr), nullptr);
        SendMessageW(edit_, WM_SETFONT, (WPARAM)font_, TRUE);
        SetWindowSubclass(edit_, EditProc, 1, (DWORD_PTR)this);
    }
    MoveWindow(edit_, kPad + 44, kPad + 9, kWidth - kPad * 2 - 56, kSearchH - 18, TRUE);

    if (editBrush_) DeleteObject(editBrush_);
    editBrush_ = CreateSolidBrush((dark_ ? DarkTheme() : LightTheme()).search);
}

void SearchWindow::Filter() {
    hits_.clear();
    if (!all_) return;
    wchar_t buf[256]{};
    if (edit_) GetWindowTextW(edit_, buf, 255);
    // 组字中的拼音（未上屏）也算查询词，否则用中文输入法时列表会一动不动
    const std::wstring raw = Lower(std::wstring(buf) + imeComp_);
    const std::wstring q = StripSeparators(raw);

    // 打分：名称前缀 3 > 拼音首字母前缀 2 > 其它命中 1，稳定排序保证同分按桌面顺序
    std::vector<std::pair<DesktopIconView*, int>> scored;
    scored.reserve(all_->size());
    for (auto* e : *all_) {
        if (!e) continue;
        if (q.empty()) {
            scored.emplace_back(e, 0);
            continue;
        }
        const std::wstring nm = Lower(e->name);
        if (nm.rfind(raw, 0) == 0) {
            scored.emplace_back(e, 3);   // 原样前缀（含空格，例如 "deep "）
            continue;
        }
        const std::wstring nc = StripSeparators(nm);
        if (nc.rfind(q, 0) == 0) {
            scored.emplace_back(e, 3);   // 忽略空格/连字符的前缀
            continue;
        }
        if (nm.find(raw) != std::wstring::npos || nc.find(q) != std::wstring::npos) {
            scored.emplace_back(e, 1);
            continue;
        }
        // 拼音首字母：既能前缀（wyy → 网易云音乐），也能中间
        const std::wstring py = PinyinInitials(e->name);
        if (py.rfind(q, 0) == 0)
            scored.emplace_back(e, 2);
        else if (py.find(q) != std::wstring::npos)
            scored.emplace_back(e, 1);
    }
    std::stable_sort(scored.begin(), scored.end(),
                     [](const auto& a, const auto& b) { return a.second > b.second; });
    for (auto& s : scored) {
        if (hits_.size() >= 60) break;
        hits_.push_back(s.first);
    }

    if (sel_ >= (int)hits_.size()) sel_ = (int)hits_.size() - 1;
    if (sel_ < 0) sel_ = 0;
}

void SearchWindow::MoveSel(int delta) {
    if (hits_.empty()) return;
    int n = (int)hits_.size();
    sel_ = ((sel_ + delta) % n + n) % n;
    Repaint();
}

void SearchWindow::Activate() {
    if (hits_.empty()) return;
    DesktopIconView* e = hits_[std::clamp(sel_, 0, (int)hits_.size() - 1)];
    Hide();
    if (e && onOpen) onOpen(e);
}

void SearchWindow::ShowPalette() {
    if (!hwnd_) return;
    dark_ = !AppsUseLightTheme();
    imeComp_.clear();
    Layout();
    Filter();
    sel_ = 0;
    if (edit_) SetWindowTextW(edit_, L"");
    Filter();

    // 居中到光标所在显示器
    POINT cur{};
    GetCursorPos(&cur);
    HMONITOR mon = MonitorFromPoint(cur, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi{};
    mi.cbSize = sizeof(mi);
    RECT lim{0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN)};
    if (mon && GetMonitorInfoW(mon, &mi)) lim = mi.rcWork;
    RECT wr{};
    GetWindowRect(hwnd_, &wr);
    const int w = wr.right - wr.left;
    const int h = wr.bottom - wr.top;
    const int x = lim.left + (lim.right - lim.left - w) / 2;
    const int y = lim.top + (lim.bottom - lim.top) / 3 - h / 3;

    SetWindowPos(hwnd_, HWND_TOPMOST, x, y, w, h, SWP_SHOWWINDOW);
    visible_ = true;
    ShowWindow(hwnd_, SW_SHOW);
    SetForegroundWindow(hwnd_);
    SetFocus(edit_);
    SendMessageW(edit_, EM_SETSEL, 0, -1);
    HookKeyboard();
    Repaint();
}

void SearchWindow::Hide() {
    if (!hwnd_ || !visible_) return;
    visible_ = false;
    UnhookKeyboard();   // 一定要卸，否则会吞掉全系统的 Esc
    imeComp_.clear();
    ShowWindow(hwnd_, SW_HIDE);
}

void SearchWindow::Toggle() {
    if (visible_) Hide();
    else ShowPalette();
}

void SearchWindow::Repaint() {
    if (!hwnd_ || !visible_) return;
    RECT cr{};
    GetClientRect(hwnd_, &cr);
    const int w = cr.right, h = cr.bottom;
    if (w <= 0 || h <= 0) return;
    if (!memDC_ || bufW_ != w || bufH_ != h) {
        if (memDC_) { DeleteDC(memDC_); memDC_ = nullptr; }
        if (dib_) { DeleteObject(dib_); dib_ = nullptr; }
        bits_ = nullptr;
        if (!CreateDib(w, h, memDC_, dib_, bits_)) return;
        bufW_ = w;
        bufH_ = h;
    }

    const Theme t = dark_ ? DarkTheme() : LightTheme();

    Bitmap target(w, h, w * 4, PixelFormat32bppRGB, reinterpret_cast<BYTE*>(bits_));
    Graphics g(&target);
    g.SetSmoothingMode(SmoothingModeAntiAlias);
    g.SetPixelOffsetMode(PixelOffsetModeHighQuality);
    g.SetTextRenderingHint(TextRenderingHintClearTypeGridFit);
    g.Clear(C(t.bg));

    // ---- 搜索行 ----
    {
        GraphicsPath p;
        AddRound(p, RectF((float)kPad, (float)kPad, (float)(w - kPad * 2), (float)kSearchH),
                 10.0f);
        SolidBrush fill(C(t.search));
        g.FillPath(&fill, &p);
        Pen pen(C(t.border), 1.0f);
        g.DrawPath(&pen, &p);

        // 放大镜
        const float cx = (float)kPad + 24.0f;
        const float cy = (float)kPad + (float)kSearchH * 0.5f;
        Pen mag(C(t.sub), 1.8f);
        g.DrawEllipse(&mag, cx - 6.5f, cy - 6.5f, 11.0f, 11.0f);
        g.DrawLine(&mag, cx + 3.5f, cy + 3.5f, cx + 8.0f, cy + 8.0f);
    }

    FontFamily family(L"Microsoft YaHei UI");
    Font fItem(&family, 15.0f, FontStyleRegular, UnitPixel);
    Font fSub(&family, 12.0f, FontStyleRegular, UnitPixel);

    StringFormat sf;
    sf.SetLineAlignment(StringAlignmentCenter);
    sf.SetFormatFlags(StringFormatFlagsNoWrap);
    sf.SetTrimming(StringTrimmingEllipsisCharacter);

    SolidBrush brText(C(t.text));
    SolidBrush brSub(C(t.sub));

    // ---- 结果列表 ----
    const int rows = RowsVisible();
    for (int i = 0; i < rows; ++i) {
        RECT r = RowRect(i);
        const bool sel = (i == sel_);
        if (sel || i == hot_) {
            GraphicsPath p;
            AddRound(p, RectF((float)r.left, (float)r.top, (float)(r.right - r.left),
                              (float)(r.bottom - r.top)),
                     8.0f);
            SolidBrush b(sel ? C(t.selBg) : CA(t.hoverBg, dark_ ? 120 : 255));
            g.FillPath(&b, &p);
            if (sel) {
                GraphicsPath bar;
                AddRound(bar,
                         RectF((float)r.left + 2.0f, (float)r.top + 11.0f, 3.0f, 20.0f), 1.5f);
                SolidBrush ab(C(t.accent));
                g.FillPath(&ab, &bar);
            }
        }

        DesktopIconView* e = hits_[i];
        const int iy = r.top + ((r.bottom - r.top) - 32) / 2;
        if (e->bmp.valid()) {
            Bitmap icon(e->bmp.width, e->bmp.height, e->bmp.width * 4,
                        PixelFormat32bppPARGB,
                        const_cast<BYTE*>(reinterpret_cast<const BYTE*>(e->bmp.pixels.data())));
            g.DrawImage(&icon, Rect(r.left + 14, iy, 32, 32));
        }
        g.DrawString(e->name.c_str(), -1, &fItem,
                     RectF((float)r.left + 58.0f, (float)r.top,
                           (float)(r.right - r.left) - 70.0f, (float)(r.bottom - r.top)),
                     &sf, &brText);
    }

    if (hits_.empty()) {
        StringFormat cf;
        cf.SetAlignment(StringAlignmentCenter);
        cf.SetLineAlignment(StringAlignmentCenter);
        g.DrawString(L"\u6ca1\u6709\u5339\u914d\u7684\u56fe\u6807", -1, &fItem,
                     RectF(0.0f, (float)kListTop, (float)w,
                           (float)(kMaxRows * kRowH)),
                     &cf, &brSub);
    }

    // ---- 底部提示 ----
    {
        StringFormat lf;
        lf.SetLineAlignment(StringAlignmentCenter);
        SolidBrush hint(C(t.sub));
        g.DrawString(L"\u2191\u2193 \u9009\u62e9   Enter \u6253\u5f00   Esc \u5173\u95ed", -1,
                     &fSub, RectF((float)kPad + 4.0f, (float)h - (float)kFooterH,
                                  (float)w - kPad * 2, (float)kFooterH),
                     &lf, &hint);
    }

    g.Flush(FlushIntentionSync);

    HDC dc = GetDC(hwnd_);
    BitBlt(dc, 0, 0, w, h, memDC_, 0, 0, SRCCOPY);
    ReleaseDC(hwnd_, dc);
}

LRESULT SearchWindow::WndProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_ERASEBKGND:
        return 1;

    case WM_PAINT: {
        PAINTSTRUCT ps{};
        BeginPaint(h, &ps);
        EndPaint(h, &ps);
        Repaint();
        return 0;
    }

    case WM_CTLCOLOREDIT:
    case WM_CTLCOLORSTATIC: {
        HDC dc = (HDC)wp;
        const Theme t = dark_ ? DarkTheme() : LightTheme();
        SetTextColor(dc, C(t.text).ToCOLORREF());
        SetBkColor(dc, C(t.search).ToCOLORREF());
        SetBkMode(dc, OPAQUE);
        return (LRESULT)(editBrush_ ? editBrush_ : GetStockObject(WHITE_BRUSH));
    }

    case WM_COMMAND:
        if (LOWORD(wp) == kEditId && HIWORD(wp) == EN_CHANGE) {
            Filter();
            Repaint();
            return 0;
        }
        break;

    case WM_MOUSEMOVE: {
        POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        int hot = -1;
        for (int i = 0; i < RowsVisible(); ++i) {
            RECT r = RowRect(i);
            if (pt.x >= r.left && pt.x < r.right && pt.y >= r.top && pt.y < r.bottom) {
                hot = i;
                break;
            }
        }
        if (hot != hot_) { hot_ = hot; Repaint(); }
        return 0;
    }

    case WM_LBUTTONDOWN: {
        POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        for (int i = 0; i < RowsVisible(); ++i) {
            RECT r = RowRect(i);
            if (pt.x >= r.left && pt.x < r.right && pt.y >= r.top && pt.y < r.bottom) {
                sel_ = i;
                Activate();
                return 0;
            }
        }
        // 点卡片外（这里没有外部，卡片就是窗口）→ 关闭
        return 0;
    }

    case WM_ACTIVATE:
        if (LOWORD(wp) == WA_INACTIVE) Hide();
        return 0;

    case WM_KEYDOWN:
        if (wp == VK_ESCAPE) { Hide(); return 0; }
        break;

    case WM_CLOSE:
        Hide();
        return 0;

    case WM_DESTROY:
        return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

bool SearchWindow::Create() {
    EnsureGdiplus();
    dark_ = !AppsUseLightTheme();

    HINSTANCE inst = GetModuleHandleW(nullptr);
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = WndProcStatic;
    wc.hInstance = inst;
    wc.lpszClassName = kClass;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = nullptr;
    RegisterClassExW(&wc);

    // WS_CLIPCHILDREN：面板是「GDI+ 画满整张位图再 BitBlt」的，没有这个样式时
    // 每次重绘都会把输入框那块一起刷掉 —— 上屏的中文就这样被"擦"成看不见。
    hwnd_ = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_TOPMOST, kClass, L"meow-desk \u641c\u7d22",
                            WS_POPUP | WS_CLIPCHILDREN, 0, 0, kWidth, 400, nullptr, nullptr,
                            inst, this);
    if (!hwnd_) return false;

    DWORD corner = 2;  // DWMWCP_ROUND
    DwmSetWindowAttribute(hwnd_, 33, &corner, sizeof(corner));
    BOOL useDark = dark_ ? TRUE : FALSE;
    DwmSetWindowAttribute(hwnd_, 20, &useDark, sizeof(useDark));

    Layout();
    return true;
}

void SearchWindow::Destroy() {
    UnhookKeyboard();
    if (edit_) {
        RemoveWindowSubclass(edit_, EditProc, 1);
        edit_ = nullptr;
    }
    if (font_) { DeleteObject(font_); font_ = nullptr; }
    if (editBrush_) { DeleteObject(editBrush_); editBrush_ = nullptr; }
    if (hwnd_) {
        SetWindowLongPtrW(hwnd_, GWLP_USERDATA, 0);
        DestroyWindow(hwnd_);
        hwnd_ = nullptr;
    }
    if (dib_) { DeleteObject(dib_); dib_ = nullptr; }
    if (memDC_) { DeleteDC(memDC_); memDC_ = nullptr; }
    bits_ = nullptr;
    bufW_ = bufH_ = 0;
    hits_.clear();
}

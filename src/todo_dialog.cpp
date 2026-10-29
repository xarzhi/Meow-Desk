#include "todo_dialog.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <ctime>
#include <utility>
#include <vector>

#include <windowsx.h>
#include <commctrl.h>   // SetWindowSubclass / DefSubclassProc
#include <dwmapi.h>     // DwmSetWindowAttribute：挡住 Win11 给窗口自己切圆角

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
const int kInitCardW = 420;  // 卡片初始宽度：详情正文要放得下一整行话
// 卡片尺寸现在是**用户的**（第 51 批：允许拖边框调整大小）：
// 窗口尺寸变化时，多出来/少掉的高度全部给文本域，宽度同理。
const int kMinCardW = 340;   // 再窄两颗按钮就挤在一起了
const int kMaxCardW = 980;   // 太宽一行字读起来累（不是硬限制，屏幕更窄时以屏幕为准）
const int kPadX = 24;
const int kPadTop = 22;
const int kTitleH = 22;
const int kAfterTitle = 16;
// 新建待办现在是**多行文本域**（用户要求「下面的 input 改成文本域」）：
// 默认 3 行，内容变多长到 6 行，再多就在框内自己滚。
const int kEditLines = 3;
const int kEditMaxLines = 6;
const int kEditPadY = 8;     // 文本域内上下留白（多行 EDIT 是从顶部排字的）
const int kEditLimit = 500;  // 一条待办最多这么多字
const int kFieldPadX = 10;
const int kAfterEdit = 20;
const int kBtnW = 92;
const int kBtnH = 32;
const int kBtnGap = 10;
const int kPadBottom = 22;
const int kDetailLineH = 21;
// 详情正文的行数上限分两档：绝对上限（再多也不给）和「屏幕放得下多少」。
// 输入改成多行文本域之后，一条待办可能有十几行 —— 详情弹窗是唯一能看全文的地方，
// 还按原来的 6 行截断就说不过去了。
const int kDetailMaxLines = 20;
const int kDetailMinLines = 3;

// 文本域行数变了 → 卡片要重新排版（见 EditProc 末尾那段「顺手量一下行数」）。
// 编号沿用原来的 +14，别和 rename_dialog 的 WM_APP+12 撞上。
const UINT kMsgRelayout = WM_APP + 14;

enum class Mode { Input, Detail };

HWND g_card = nullptr;
HWND g_edit = nullptr;   // 两个模式都有（详情未完成时是可编辑的）
Mode g_mode = Mode::Input;
std::wstring g_title;
std::wstring g_text;     // Detail 模式的**原始**正文（关窗时用来判断有没有改过）
bool g_done = false;     // Detail 模式：这条待办当前是不是已完成
// Detail 且已完成 = 正文只读（用户要求「完成了就不可编辑」）。
// 只读时也不让 Ctrl+Enter 去切完成状态 —— 已完成的那条按回车应该是关窗。
bool g_readOnly = false;
long long g_created = 0;   // Detail：创建时间（Unix 秒；0 = 老数据不知道）
std::function<void(const std::wstring&)> g_onOk;
std::function<void(const std::wstring&)> g_onSave;   // Detail 可编辑且内容改过才回调
std::function<void()> g_onToggle;
std::function<void()> g_onDone;

bool g_activated = false;
bool g_dark = false;
int g_hover = -1;        // 0 = 次要按钮，1 = 主按钮，-1 = 空白
int g_press = -1;

// 拖窗口：**只有头部（标题那一行）**能按着拖（用户要求），正文/按钮区按了不动。
// 光标也不做特殊处理 —— 用户明确说「不需要改变鼠标样式」。
bool g_dragArmed = false;   // 在头部按下了（还没确定是不是拖动）
bool g_dragging = false;    // 已经超过死区，正在搬
POINT g_dragAnchor{};       // 按下时的光标（屏幕坐标）
POINT g_dragWin0{};         // 按下时的卡片左上角

int g_cardW = kInitCardW;   // 卡片（不含阴影）的宽
int g_cardH = 0;            // 卡片（不含阴影）的高 —— 由内容或用户拖边框决定
// 用户拖过一次边框之后就不再按内容自动长高了：尺寸从此由他说了算，
// 否则用户刚拉大、一打字又被程序缩回去。
bool g_userSized = false;
// 正处在系统的「拖动边框改大小」模态循环里（WM_ENTERSIZEMOVE ~ WM_EXITSIZEMOVE）。
// 这期间 WM_SIZE 会连发几十次，不能让内容排版插进来抢尺寸。
bool g_sizing = false;
int g_sizeW0 = 0;           // 进循环时的卡片尺寸，退出时比一比就知道有没有真的改过
int g_sizeH0 = 0;
int g_textH = 0;         // Detail 模式正文量出来的高度（换算初始卡片高度用）
int g_detailCap = kDetailLineH * 8;   // Detail 正文最多这么高（ShowDetail 按屏幕算）
int g_editH = 0;         // 文本域的高度（= 卡片高 - 那圈固定的间距，见 ChromeH）
int g_editLines = 1;     // 上一帧文本域的显示行数（用来判断要不要重排版）
RECT g_rcField{};
RECT g_rcEdit{};
RECT g_rcOk{};
RECT g_rcCancel{};

HDC g_memDC = nullptr;
HBITMAP g_dib = nullptr;
void* g_bits = nullptr;
int g_bufW = 0;
int g_bufH = 0;

HBRUSH g_brEdit = nullptr;       // 可编辑文本域的底刷（缓存，不删）
HBRUSH g_brReadOnly = nullptr;   // 只读文本域的底刷

// 文本域底色。只读（= 已完成的详情）给一层很淡的灰 —— 光靠「点了打不进字」
// 用户才知道不能改，太晚了；底色浅灰一眼就能看出来这块是只读的。
COLORREF EditBgColor() {
    if (g_readOnly) return g_dark ? RGB(30, 30, 35) : RGB(245, 245, 247);
    return g_dark ? RGB(35, 35, 41) : RGB(255, 255, 255);
}

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

// 文本域要多高。行数按「显示行」（ES_MULTILINE 自己换行后的行数）算，
// 夹在 [kEditLines, kEditMaxLines] 之间 —— 少于 3 行看着不像文本域，
// 多于 6 行卡片就快顶到屏幕边了，再多的内容让 EDIT 自己滚。
int EditHeightFor(int lines) {
    const int n = std::clamp(lines, kEditLines, kEditMaxLines);
    return FieldLineHeight() * n + kEditPadY * 2;
}

// 卡片里「文本域以外」那一圈固定高度：上内边距 + 标题 + 间距 + 按钮行 + 下内边距。
// 卡片高度减去它就是文本域的高度 —— 换句话说，用户拖边框多出来的高度全给文本域。
int ChromeH() { return kPadTop + kTitleH + kAfterTitle + kAfterEdit + kBtnH + kPadBottom; }
int MinCardH() { return ChromeH() + EditHeightFor(kEditLines); }

// 文本域当前显示几行。EM_GETLINECOUNT 对多行 EDIT 返回的就是**显示行**（含折行），
// 单行 EDIT 恒等于 1（这里只有多行，放心用）。
int EditLineCountNow() {
    if (!g_edit) return 1;
    const LRESULT n = SendMessageW(g_edit, EM_GETLINECOUNT, 0, 0);
    return n > 0 ? (int)n : 1;
}

// 「创建于 2026-10-29 15:48」。created 存的是 Unix 秒（UTC），显示按本地时区换算。
// <= 0 表示没有（老数据迁移时已经回填过一次，正常走不到这里）。
std::wstring CreatedLabel(long long secs) {
    if (secs <= 0) return std::wstring();
    const time_t t = (time_t)secs;
    tm lt{};
    if (localtime_s(&lt, &t) != 0) return std::wstring();
    wchar_t buf[80]{};
    // 创建于 YYYY-MM-DD HH:MM
    swprintf_s(buf, L"\u521b\u5efa\u4e8e %04d-%02d-%02d %02d:%02d", lt.tm_year + 1900,
               lt.tm_mon + 1, lt.tm_mday, lt.tm_hour, lt.tm_min);
    return std::wstring(buf);
}

HBRUSH EditBgBrush() {
    HBRUSH& slot = g_readOnly ? g_brReadOnly : g_brEdit;
    if (!slot) slot = CreateSolidBrush(EditBgColor());
    return slot;
}

// 详情正文「按卡片宽度换行后有多高」。卡片高度要在建缓冲之前定下来，所以必须先量。
int MeasureDetailHeight(const std::wstring& s) {
    // 宽度要按**真正显示文字的那块**算：卡片内宽减掉文本域左右内边距，再减掉右侧
    // 那条竖向滚动条。按卡片内宽量会少折一行 → 建出来的 EDIT 一出场就带滚动条。
    const int textW = g_cardW - kPadX * 2 - kFieldPadX * 2 - 16;
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
    return std::clamp(h, kDetailLineH, g_detailCap);
}

// 两个模式是同一套骨架：标题 + 文本域 + 按钮。差别只在文本域里的内容能不能改、
// 按钮写什么、以及标题右边那颗状态徽标。
// 高度不再是算出来的 —— 它是 g_cardH（用户能拖边框改），Layout 里反过来推文本域高度。
void EnsureBuffer() {
    const int w = g_cardW + kShadow * 2;
    const int h = g_cardH + kShadow * 2;
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
    // 文本域吃掉剩下的所有高度：用户把窗口拉高多少，能看到的正文就多多少。
    g_editH = std::max(EditHeightFor(1), g_cardH - ChromeH());
    const int cx = kShadow, cy = kShadow;

    const int fieldTop = cy + kPadTop + kTitleH + kAfterTitle;
    g_rcField = RECT{cx + kPadX, fieldTop, cx + g_cardW - kPadX, fieldTop + g_editH};

    // 多行 EDIT 是从客户区顶部往下排字的，所以上下各留 kEditPadY 就正好；
    // 别再像单行那样「只占一行高、垂直居中」—— 那套是为了单行居中，
    // 换成文本域之后按行高算会把下面的行切掉。
    g_rcEdit = RECT{g_rcField.left + kFieldPadX, fieldTop + kEditPadY,
                    g_rcField.right - kFieldPadX, fieldTop + g_editH - kEditPadY};

    const int btnTop = cy + g_cardH - kPadBottom - kBtnH;
    const int okRight = cx + g_cardW - kPadX;
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

// 「头部」= 标题那一行（含上方内边距）。用户要求：**只有头部**能按着拖，
// 而且不要改鼠标样式。正文区本来就是另一个顶层 EDIT（点它这边收不到），
// 这一条是把标题下面那条空缝和按钮行也排除掉。
bool InDragHeader(POINT c) {
    if (c.y < kShadow) return false;
    return c.y <= kShadow + kPadTop + kTitleH;
}

void Paint() {
    if (!g_memDC || !g_bits || g_bufW <= 0 || g_bufH <= 0) return;
    // 底色不能 memset 0：分层窗口的鼠标命中是**按 alpha 判**的，alpha=0 的像素点击会
    // 直接穿透到后面的窗口。可拖边框的那一圈恰好落在阴影留白里（真正画出阴影的只有
    // 卡片外 6px 左右，再外面几圈是全透明的），alpha=0 的话那条边根本点不到。
    // 铺一层 alpha=1 的黑：肉眼完全看不出来（0.4%），但 WM_NCHITTEST 收得到。
    {
        DWORD* px = static_cast<DWORD*>(g_bits);
        std::fill(px, px + (size_t)g_bufW * (size_t)g_bufH, 0x01000000u);
    }

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

        const RectF body((float)kShadow, (float)kShadow, (float)g_cardW, (float)g_cardH);

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

        StringFormat oneLine;
        oneLine.SetLineAlignment(StringAlignmentCenter);
        oneLine.SetFormatFlags(StringFormatFlagsNoWrap);
        oneLine.SetTrimming(StringTrimmingEllipsisCharacter);

        // 标题
        {
            SolidBrush tb(fg);
            // 详情模式的标题右边挂着状态徽标，标题的可用宽度要把那块**让出来** ——
            // 窗口现在能被拖窄（最小 340），不让的话「待办详情」四个字会压到「未完成」上。
            const float badgeReserve = (g_mode == Mode::Detail) ? 66.0f : 0.0f;   // 56 + 10
            g.DrawString(g_title.c_str(), -1, &fTitle,
                         RectF((float)kShadow + kPadX, (float)kShadow + kPadTop,
                               (float)g_cardW - kPadX * 2 - badgeReserve, (float)kTitleH),
                         &oneLine, &tb);

            // 详情模式：标题右边一个小徽标标状态 —— 弹窗里除了正文，用户最想知道
            // 就是「这条到底做完了没有」。不占额外高度，直接挂在标题这一行右端。
            if (g_mode == Mode::Detail) {
                Font fBadge(&fam, 12.0f, FontStyleRegular, UnitPixel);
                const float bw = 56.0f, bh = 20.0f;
                const float bx = (float)(kShadow + g_cardW - kPadX) - bw;
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

        // 底槽：圆角 + 1px 描边。真正的字由嵌在里面的 EDIT 画 —— 两个模式都是，
        // 详情模式的正文现在也是一个 EDIT（未完成时可编辑）。
        {
            const RectF field((float)g_rcField.left, (float)g_rcField.top,
                              (float)(g_rcField.right - g_rcField.left),
                              (float)(g_rcField.bottom - g_rcField.top));
            GraphicsPath fp;
            AddRoundRect(fp, field, 8.0f);
            // 底色必须和 WM_CTLCOLOREDIT 给 EDIT 的刷子一模一样，否则会有色差补丁
            SolidBrush fb(g_readOnly ? (g_dark ? 0xFF1E1E23 : 0xFFF5F5F7)
                                     : (g_dark ? 0xFF232329 : 0xFFFFFFFF));
            g.FillPath(&fb, &fp);
            Pen fpen(light ? 0xFFD6D6DC : 0xFF3E3E46, 1.0f);
            g.DrawPath(&fpen, &fp);

            // 文本域和按钮之间那条 20px 的空缝：左边放创建时间（详情模式），
            // 右边放快捷键提示。两头都不占额外高度，也不会把按钮挤窄。
            Font fHint(&fam, 12.0f, FontStyleRegular, UnitPixel);
            const RectF hr((float)kShadow + kPadX,
                           (float)(kShadow + kPadTop + kTitleH + kAfterTitle + g_editH),
                           (float)g_cardW - kPadX * 2, (float)kAfterEdit);
            SolidBrush hb{light ? Color(0x88000000) : Color(0x88FFFFFF)};

            if (g_mode == Mode::Detail) {
                const std::wstring when = CreatedLabel(g_created);
                if (!when.empty()) {
                    StringFormat lf;
                    lf.SetAlignment(StringAlignmentNear);
                    lf.SetLineAlignment(StringAlignmentCenter);
                    lf.SetFormatFlags(StringFormatFlagsNoWrap);
                    g.DrawString(when.c_str(), -1, &fHint, hr, &lf, &hb);
                }
            }

            // 详情里没完成才提「Ctrl+Enter 完成」—— 已完成的那条按回车是关窗，
            // 提了反而误导。
            const wchar_t* tip = nullptr;
            if (g_mode == Mode::Input)
                tip = L"Enter \u6362\u884c \u00b7 Ctrl+Enter \u786e\u5b9a";   // Enter 换行 · Ctrl+Enter 确定
            else if (!g_readOnly)
                tip = L"Ctrl+Enter \u5b8c\u6210";   // Ctrl+Enter 完成
            if (tip) {
                StringFormat rf;
                rf.SetAlignment(StringAlignmentFar);
                rf.SetLineAlignment(StringAlignmentCenter);
                rf.SetFormatFlags(StringFormatFlagsNoWrap);
                g.DrawString(tip, -1, &fHint, hr, &rf, &hb);
            }
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
void Teardown(std::function<void(const std::wstring&)>& ok,
              std::function<void(const std::wstring&)>& save, std::function<void()>& toggle,
              std::function<void()>& done, std::wstring& text) {
    HWND card = g_card;
    HWND edit = g_edit;
    g_card = nullptr;
    g_edit = nullptr;
    g_hover = -1;
    g_press = -1;
    g_activated = false;
    g_dragArmed = false;
    g_dragging = false;
    g_editLines = 1;

    ok = std::move(g_onOk);
    save = std::move(g_onSave);
    toggle = std::move(g_onToggle);
    done = std::move(g_onDone);
    g_onOk = nullptr;
    g_onSave = nullptr;
    g_onToggle = nullptr;
    g_onDone = nullptr;
    text = g_text;

    if (edit) DestroyWindow(edit);   // 先拆被拥有的那个，再拆卡片
    if (card) DestroyWindow(card);
}

// 多行 EDIT 交出来的是 CRLF；统一存成 LF —— 那样详情弹窗的排版、行内摊平、
// 以及以后可能的导出都不用再各判一次。
std::wstring NormalizeNewlines(const std::wstring& s) {
    std::wstring out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        const wchar_t c = s[i];
        if (c == L'\r') {
            if (i + 1 < s.size() && s[i + 1] == L'\n') ++i;
            out.push_back(L'\n');
        } else {
            out.push_back(c);
        }
    }
    return out;
}

// 读文本域里的文字：CRLF 收成 LF、去掉首尾空白。空内容返回空字符串。
// 新建（提交）和详情（改完保存）两条路都用这一份，语义才不会分叉。
std::wstring ReadEditText() {
    if (!g_edit) return std::wstring();
    // 先问长度再取：文本域可能是多行（几百个字符），固定栈缓冲会被截断，
    // 而 WM_GETTEXT 只会老老实实拷进缓冲那么多。
    std::wstring s;
    const int n = GetWindowTextLengthW(g_edit);
    if (n > 0) {
        std::vector<wchar_t> buf((size_t)n + 2, L'\0');
        const int got =
            (int)SendMessageW(g_edit, WM_GETTEXT, (WPARAM)(n + 1), (LPARAM)buf.data());
        if (got > 0) s.assign(buf.data(), (size_t)got);
    }
    s = NormalizeNewlines(s);
    size_t a = 0, b = s.size();
    while (a < b && (s[a] == L' ' || s[a] == L'\t' || s[a] == L'\r' || s[a] == L'\n')) ++a;
    while (b > a && (s[b - 1] == L' ' || s[b - 1] == L'\t' || s[b - 1] == L'\r' ||
                     s[b - 1] == L'\n'))
        --b;
    return s.substr(a, b - a);
}

// 新建待办：commit = true 时把输入框里的文字交出去（空字符串 = 当没写）
void FinishInput(bool commit) {
    if (!g_card) return;
    const std::wstring input = commit ? ReadEditText() : std::wstring();

    std::function<void(const std::wstring&)> ok, save;
    std::function<void()> toggle, done;
    std::wstring ignored;
    Teardown(ok, save, toggle, done, ignored);

    // 回调放在窗口销毁之后：里面会存配置、重画盒子
    if (commit && !input.empty() && ok) ok(input);
    if (done) done();
}

// 详情：toggle = true 表示点了主按钮（完成 / 恢复）。
// 未完成的详情正文是可编辑的 —— 关窗时把改过的内容交回去（onSave）。
void FinishDetail(bool toggleNow) {
    if (!g_card) return;

    // 空内容不保存：清空大概率是手滑（真想删有 × 和右键菜单），
    // 把一条待办悄悄变成空白，比「改不上去」更让人难受。
    std::wstring edited;
    const bool canEdit = (g_edit && !g_readOnly);
    if (canEdit) edited = ReadEditText();
    const bool changed = canEdit && !edited.empty() && edited != g_text;

    std::function<void(const std::wstring&)> ok, save;
    std::function<void()> toggle, done;
    std::wstring ignored;
    Teardown(ok, save, toggle, done, ignored);

    // **先切完成状态，再存文字**：两个回调都是靠「下标 + 原文字指纹」找到那条的，
    // 保存会把文字改掉、指纹随之失效，反过来就切不动了。
    if (toggleNow && toggle) toggle();
    if (changed && save) save(edited);
    if (done) done();
}

void CloseCard() {
    if (g_mode == Mode::Detail)
        FinishDetail(false);
    else
        FinishInput(false);
}

// 文本域里按 Ctrl+Enter 该干什么。三个状态三种答案：
//   新建 → 提交；详情未完成 → 直接完成；详情已完成 → 什么都不做
//（只读的时候提「再完成一次」没意义，完成/恢复请点按钮）。
void SubmitFromEdit() {
    if (g_mode == Mode::Detail) {
        if (!g_readOnly) FinishDetail(true);
    } else {
        FinishInput(true);
    }
}

// ---------------------------------------------------------------------------
// 拖动 / 文本域重排版
// ---------------------------------------------------------------------------

// 卡片至少留这么多像素在可见区里 —— 拖到屏幕外找不回来是很烦的事。
const int kKeepVisible = 90;

void ClampToWorkArea(int& x, int& y) {
    const POINT c{x + g_bufW / 2, y + g_bufH / 2};
    HMONITOR mon = MonitorFromPoint(c, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi{};
    mi.cbSize = sizeof(mi);
    RECT lim{0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN)};
    if (mon && GetMonitorInfoW(mon, &mi)) lim = mi.rcWork;
    // 卡片比工作区还大时（极端缩放）这个值要退化成「顶住左上角」，别算出负数
    const int kx = std::min(kKeepVisible, g_bufW);
    const int ky = std::min(kKeepVisible, g_bufH);
    if (x > (int)lim.right - kx) x = (int)lim.right - kx;
    if (x + g_bufW < (int)lim.left + kx) x = (int)lim.left + kx - g_bufW;
    if (y > (int)lim.bottom - ky) y = (int)lim.bottom - ky;
    if (y + g_bufH < (int)lim.top + ky) y = (int)lim.top + ky - g_bufH;
}

// 文本域行数变了：卡片高度跟着变、缓冲重建、按钮整体下移、EDIT 跟着改尺寸。
// **卡片顶边不动**，只往下长 —— 打字的时候窗口自己上下跳会很难受。
//
// 第 51 批：尺寸现在也能由用户拖边框决定（g_userSized）。他说了算之后就不再抢了，
// 否则「刚拉大 → 打个字 → 被缩回 3 行」。
void RelayoutInput() {
    if (!g_card || g_mode != Mode::Input || !g_edit) return;
    if (g_userSized || g_sizing) return;
    const int wantH = ChromeH() + EditHeightFor(EditLineCountNow());
    if (wantH == g_cardH) return;

    RECT wr{};
    if (!GetWindowRect(g_card, &wr)) return;
    g_cardH = wantH;
    Layout();
    EnsureBuffer();
    if (!g_memDC) return;

    // 这里不用 ApplyWindowSize：它是给 WM_SIZE 用的（从窗口尺寸倒推卡片尺寸），
    // 而这条路径是「先定卡片高度、再去改窗口」，方向相反。
    SetWindowPos(g_card, HWND_TOPMOST, wr.left, wr.top, g_bufW, g_bufH,
                 SWP_NOACTIVATE | SWP_SHOWWINDOW);
    if (g_edit) {
        SetWindowPos(g_edit, HWND_TOPMOST, wr.left + (int)g_rcEdit.left,
                     wr.top + (int)g_rcEdit.top, (int)(g_rcEdit.right - g_rcEdit.left),
                     (int)(g_rcEdit.bottom - g_rcEdit.top),
                     SWP_NOACTIVATE | SWP_SHOWWINDOW);
    }
    Paint();
    Present();
}

// 窗口被改成 cw×ch（卡片尺寸，不含阴影）之后重新排版并重贴位图。
// 拖边框（WM_SIZE 连发）、显示器的 DPI 变化，最后都汇到这一个入口。
// 文本域是「卡片拥有的顶层弹出窗口」不是子控件，**它不会跟着动**，必须手动搬。
// 返回 true 表示真的按新尺寸重排了（尺寸没变就什么都不做，返回 false）。
bool ApplyWindowSize(int cw, int ch) {
    if (!g_card) return false;
    if (cw < kMinCardW) cw = kMinCardW;
    if (ch < MinCardH()) ch = MinCardH();
    if (cw == g_cardW && ch == g_cardH) return false;

    RECT wr{};
    if (!GetWindowRect(g_card, &wr)) return false;

    g_cardW = cw;
    g_cardH = ch;
    Layout();
    EnsureBuffer();
    if (!g_memDC) return false;

    if (g_edit) {
        SetWindowPos(g_edit, HWND_TOPMOST, wr.left + (int)g_rcEdit.left,
                     wr.top + (int)g_rcEdit.top, (int)(g_rcEdit.right - g_rcEdit.left),
                     (int)(g_rcEdit.bottom - g_rcEdit.top),
                     SWP_NOACTIVATE | SWP_SHOWWINDOW);
    }
    Paint();
    Present();
    return true;
}

// ---------------------------------------------------------------------------
// 消息处理
// ---------------------------------------------------------------------------
// 哪些消息过后需要重新量一次「显示了几行」。**必须是白名单**：
// 量行数本身要 SendMessage(EM_GETLINECOUNT) 回到这个子类过程里，
// 如果对每条消息都量，EM_GETLINECOUNT 自己又会触发下一次测量 —— 无限递归。
bool MayChangeText(UINT m) {
    switch (m) {
    case WM_CHAR:
    case WM_KEYDOWN:
    case WM_KEYUP:
    case WM_PASTE:
    case WM_CUT:
    case WM_CLEAR:
    case WM_UNDO:
    case WM_SETTEXT:
    case WM_IME_CHAR:
    case WM_IME_COMPOSITION:
        return true;
    default:
        return false;
    }
}

LRESULT CALLBACK EditProc(HWND h, UINT m, WPARAM w, LPARAM l, UINT_PTR, DWORD_PTR) {
    const bool watch = MayChangeText(m);
    switch (m) {
    case WM_KEYDOWN:
        if (w == VK_RETURN) {
            // 文本域里单独一个 Enter 是**换行**（ES_WANTRETURN），提交要 Ctrl+Enter。
            // 用 GetKeyState 而不是查 WM_CHAR 的字符，和输入法 / 键盘布局无关。
            if (GetKeyState(VK_CONTROL) & 0x8000) {
                SubmitFromEdit();
                return 0;
            }
            break;
        }
        if (w == VK_ESCAPE) {
            // 详情未完成时 Esc = 关窗，但**改动照样保存**（这里不叫「取消」，
            // 弹窗里只有两颗按钮，没有一个叫取消）。走 CloseCard 统一收尾。
            CloseCard();
            return 0;
        }
        break;
    case WM_CHAR:
        // 多行 EDIT 收到 '\r' 就是插一个换行，千万别拦（拦了文本域就没法换行了）；
        // Esc 还是会「叮」一声，那个要吃掉。
        if (w == 27) return 0;
        // 只读时把字符吞掉：ES_READONLY 自己会拒绝，但会「叮」一声。
        // Ctrl+C / Ctrl+A 走 WM_KEYDOWN，不受这里影响。
        if (g_readOnly) return 0;
        break;
    case WM_GETDLGCODE:
        return DLGC_WANTALLKEYS;
    case WM_SETFOCUS:
        g_activated = true;   // 真正拿到键盘焦点才算立住
        break;
    case WM_KILLFOCUS:
        // 以前这里会把「焦点跑了就关窗」的检查 Post 给卡片。第 51 批起窗口要在任务栏
        // 留一个 tab（用户要求），点别的程序就自我销毁等于那个 tab 白加了，检查已去掉。
        break;
    case WM_NCDESTROY:
        RemoveWindowSubclass(h, EditProc, 1);
        break;
    }
    const LRESULT r = DefSubclassProc(h, m, w, l);

    // 顺手量一下「现在显示几行」：内容一变（打字 / 粘贴 / 输入法上屏 / 删行）
    // 卡片就得重新排版。**特意不走 EN_CHANGE** —— EDIT 是个 WS_POPUP 的 owned
    // 窗口，通知到底送到 owner 还是送丢，不同 Windows 版本上不保证；
    // 自己量一次行数是确定可靠的（EM_GETLINECOUNT 是同线程即时消息，很便宜）。
    if (watch && g_card && g_edit && m != WM_NCDESTROY) {
        const int n = EditLineCountNow();
        if (n != g_editLines) {
            g_editLines = n;
            PostMessageW(g_card, kMsgRelayout, 0, 0);
        }
    }
    return r;
}

LRESULT CALLBACK CardProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
    case kMsgRelayout:
        RelayoutInput();
        return 0;

    // ---- 有边框但看不见边框（第 51 批：允许拖边框改大小）----------------------
    // 加了 WS_THICKFRAME 才有「拖四条边」的能力，但那样 DefWindowProc 会真的算出一圈
    // 非客户区：`UpdateLayeredWindow` 是按**窗口矩形**贴图的，客户区一旦比窗口小，
    // 整张卡片就会往下右各错开几像素。所以直接把客户区撑满整个窗口 ——
    // 边框只剩「拿来拖」这一个作用，视觉上仍是那张无边框圆角卡片。
    case WM_NCCALCSIZE:
        if (w) return 0;
        break;

    // 边框既然被吃掉了，WM_NCHITTEST 就得自己判：客户区以外的**整圈阴影留白**都算边框。
    // 正好卡片本体是从 kShadow 处开始的，留白宽度就是 kShadow，所以中间没有「点了没反应」
    // 的死区。其余一律交回 HTCLIENT：头部拖动是自己实现的（HTCAPTION 会让系统接管拖动，
    // 而文本域是独立顶层窗口、不会跟着窗口走，用系统拖动会把输入框留在原地）。
    case WM_NCHITTEST: {
        if (g_bufW <= 0 || g_bufH <= 0) break;
        POINT pt{GET_X_LPARAM(l), GET_Y_LPARAM(l)};   // 这里是屏幕坐标
        ScreenToClient(h, &pt);
        const bool left = pt.x < kShadow;
        const bool right = pt.x >= g_bufW - kShadow;
        const bool top = pt.y < kShadow;
        const bool bottom = pt.y >= g_bufH - kShadow;
        if (top && left) return HTTOPLEFT;
        if (top && right) return HTTOPRIGHT;
        if (bottom && left) return HTBOTTOMLEFT;
        if (bottom && right) return HTBOTTOMRIGHT;
        if (left) return HTLEFT;
        if (right) return HTRIGHT;
        if (top) return HTTOP;
        if (bottom) return HTBOTTOM;
        return HTCLIENT;
    }

    case WM_GETMINMAXINFO: {
        auto* mmi = reinterpret_cast<MINMAXINFO*>(l);
        if (!mmi) break;
        DefWindowProcW(h, m, w, l);   // 先让系统把默认值（屏幕尺寸等）填上
        mmi->ptMinTrackSize.x = kMinCardW + kShadow * 2;
        mmi->ptMinTrackSize.y = MinCardH() + kShadow * 2;
        // 太宽一行字读起来累。默认值比这个还小时（小屏幕/高缩放）以默认值为准。
        const int maxW = kMaxCardW + kShadow * 2;
        if (mmi->ptMaxTrackSize.x > maxW) mmi->ptMaxTrackSize.x = maxW;
        return 0;
    }

    case WM_SIZE: {
        // 客户区 = 整窗，所以窗口尺寸减掉两圈阴影就是卡片尺寸
        if (!g_card || w == SIZE_MINIMIZED) return 0;
        const bool relaid = ApplyWindowSize((int)LOWORD(l) - kShadow * 2,
                                            (int)HIWORD(l) - kShadow * 2);
        // 从最小化恢复时尺寸没变（relaid=false），但分层窗口的表面可能是空的，补一次
        if (!relaid && w == SIZE_RESTORED) {
            Paint();
            Present();
        }
        return 0;
    }

    case WM_ENTERSIZEMOVE:
        g_sizing = true;
        g_sizeW0 = g_cardW;
        g_sizeH0 = g_cardH;
        return 0;

    case WM_EXITSIZEMOVE:
        g_sizing = false;
        // 真的改过尺寸才算「用户定了大小」，之后不再按内容自动长高
        if (g_cardW != g_sizeW0 || g_cardH != g_sizeH0) g_userSized = true;
        return 0;

    case WM_SYSCOMMAND:
        // 分层窗口最大化会散架（客户区 = 整窗，会盖住任务栏），这条直接不给
        if ((w & 0xFFF0) == SC_MAXIMIZE) return 0;
        break;

    case WM_ACTIVATE:
        if (LOWORD(w) != WA_INACTIVE) g_activated = true;
        return 0;

    case WM_MOUSEACTIVATE:
        return MA_ACTIVATE;

    case WM_KEYDOWN:
        // 一般按键都走 EDIT 的 EditProc（它有焦点）。这里只兜「焦点不在文本域」
        // 的漏网情况，语义和 EditProc 保持一致，别在两处写出两种行为。
        if (g_mode == Mode::Detail) {
            if (w == VK_ESCAPE) {
                CloseCard();
                return 0;
            }
            if (w == VK_RETURN) {
                SubmitFromEdit();
                return 0;
            }
            return 0;
        }
        break;

    case WM_MOUSEMOVE: {
        POINT pt{GET_X_LPARAM(l), GET_Y_LPARAM(l)};
        if (g_dragArmed) {
            // 拖动期间用**屏幕坐标**算位移：客户区坐标会跟着窗口一起动
            POINT cur{};
            GetCursorPos(&cur);
            const int dx = cur.x - g_dragAnchor.x;
            const int dy = cur.y - g_dragAnchor.y;
            if (!g_dragging) {
                // 3px 死区：手一抖不该把窗口挪歪（点一下别处也不该动窗口）
                if (dx > -3 && dx < 3 && dy > -3 && dy < 3) return 0;
                g_dragging = true;
            }
            int nx = g_dragWin0.x + dx;
            int ny = g_dragWin0.y + dy;
            ClampToWorkArea(nx, ny);
            // 分层窗口「画什么」由 Present 决定、「放哪儿」由 SetWindowPos 决定，
            // 纯移动不用重贴位图（每帧重贴反而会闪）。
            SetWindowPos(h, HWND_TOPMOST, nx, ny, 0, 0, SWP_NOSIZE | SWP_NOACTIVATE);
            if (g_edit)
                SetWindowPos(g_edit, HWND_TOPMOST, nx + (int)g_rcEdit.left,
                             ny + (int)g_rcEdit.top, 0, 0, SWP_NOSIZE | SWP_NOACTIVATE);
            return 0;
        }
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
            // 头部之外不给任何特殊光标 —— 用户明确说拖动「不需要改变鼠标样式」，
            // 光标交给类里注册的箭头。
        }
        break;
    }

    case WM_LBUTTONDOWN: {
        POINT pt{GET_X_LPARAM(l), GET_Y_LPARAM(l)};
        g_press = HitTest(pt);
        if (g_press >= 0) {
            SetCapture(h);
        } else if (InDragHeader(pt)) {
            // 头部按下 = 准备拖窗口。此时先不算「在拖」，动过 3px 才算。
            RECT wr{};
            if (GetWindowRect(h, &wr)) {
                g_dragArmed = true;
                g_dragging = false;
                GetCursorPos(&g_dragAnchor);
                g_dragWin0 = POINT{(int)wr.left, (int)wr.top};
                SetCapture(h);
            }
            // 点头部会把焦点从文本域抢走，还回去，否则拖完打不了字
            if (g_edit) SetFocus(g_edit);
        }
        return 0;
    }

    case WM_LBUTTONUP: {
        POINT pt{GET_X_LPARAM(l), GET_Y_LPARAM(l)};
        g_dragArmed = false;
        g_dragging = false;
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
        SetBkColor(dc, EditBgColor());
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

// 卡片该摆在哪个屏幕的哪块可用区。屏幕取 owner 所在那块（待办盒可能在副屏上），
// 没有 owner 就取鼠标所在那块。
RECT WorkAreaFor(HWND owner) {
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
    return lim;
}

// 卡片摆放：**居中于屏幕**（用户明确要求「在屏幕中央」）。
// 居中于盒子是不对的 —— 盒子本身就在屏幕角落，卡片贴着角落出来很别扭。
void PlaceAndShow(HWND owner) {
    const RECT lim = WorkAreaFor(owner);

    int x = ((int)lim.left + (int)lim.right - g_bufW) / 2;
    int y = ((int)lim.top + (int)lim.bottom - g_bufH) / 2;
    // 工作区比卡片还小（极端缩放）时至少别把卡片顶到屏幕外
    if (x < (int)lim.left) x = (int)lim.left;
    if (y < (int)lim.top) y = (int)lim.top;

    // 窗口样式（第 51 批）：
    // - **去掉 WS_EX_TOOLWINDOW**：工具窗口在任务栏上永远不会有按钮，而用户要求
    //   「这个窗口要在任务栏创建 tab」。这张卡片本来就是无 owner 的顶层窗口，
    //   去掉这一位任务栏按钮自然就出来了。
    // - **加 WS_THICKFRAME**：拿到拖四条边改大小的能力。边框本身在 WM_NCCALCSIZE
    //   里被吃掉了，看到的仍然是那张无边框圆角卡片。
    // - WS_MINIMIZEBOX | WS_SYSMENU：任务栏右键菜单里的「最小化 / 关闭」才可用，
    //   不然那个 tab 只有「显示」一个用处。最大化由 WM_SYSCOMMAND 挡掉。
    g_card = CreateWindowExW(WS_EX_LAYERED | WS_EX_TOPMOST, kCardClass, L"",
                             WS_POPUP | WS_THICKFRAME | WS_MINIMIZEBOX | WS_SYSMENU, x, y,
                             g_bufW, g_bufH, nullptr, nullptr, GetModuleHandleW(nullptr),
                             nullptr);
    if (!g_card) return;

    // 有了 WS_THICKFRAME，Win11 会想按「带标题栏的窗口」给这个窗口自己切个圆角 ——
    // 那会把我们画好的阴影四角削掉。明确告诉它别动（1 = DWMWCP_DONOTROUND）。
    {
        DWORD corner = 1;
        DwmSetWindowAttribute(g_card, 33 /*DWMWA_WINDOW_CORNER_PREFERENCE*/, &corner,
                              sizeof(corner));
    }

    // 两个模式都有文本域：新建可编辑；详情在「未完成」时可编辑、完成了只读
    // （用户要求：已完成的那条不允许改内容）。
    // 输入框是「卡片拥有的顶层弹出窗口」：分层窗口不能放子控件（不会被合成），
    // owned 窗口永远压在 owner 上面，而且键盘焦点 / 光标 / 输入法全是系统原生的。
    // 多行：ES_MULTILINE + ES_WANTRETURN（Enter 换行）+ WS_VSCROLL/ES_AUTOVSCROLL
    // （超出行数时能滚）。滚动条常驻（内容不够时灰着），换来的是「写到 500 字也翻得动」。
    DWORD st = WS_POPUP | WS_VISIBLE | ES_MULTILINE | ES_WANTRETURN | ES_AUTOVSCROLL |
               WS_VSCROLL | ES_NOHIDESEL;
    if (g_readOnly) st |= ES_READONLY;
    g_edit = CreateWindowExW(
        WS_EX_TOOLWINDOW, L"EDIT", L"", st, x + (int)g_rcEdit.left, y + (int)g_rcEdit.top,
        (int)(g_rcEdit.right - g_rcEdit.left), (int)(g_rcEdit.bottom - g_rcEdit.top), g_card,
        nullptr, GetModuleHandleW(nullptr), nullptr);
    if (g_edit) {
        SendMessageW(g_edit, WM_SETFONT, (WPARAM)DialogFont(), TRUE);
        SendMessageW(g_edit, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN, MAKELPARAM(0, 0));
        SendMessageW(g_edit, EM_SETLIMITTEXT, (WPARAM)kEditLimit, 0);
        SetWindowSubclass(g_edit, EditProc, 1, 0);
        g_editLines = EditLineCountNow();
    }

    ShowWindow(g_card, SW_SHOWNOACTIVATE);   // 先真的「显示」：任务栏按钮靠这个出现
    SetWindowPos(g_card, HWND_TOPMOST, x, y, g_bufW, g_bufH, SWP_NOACTIVATE);
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
    // **前台要给卡片本身**：任务栏按钮的高亮跟的是前台窗口，前台给了文本域的话
    // 那个 tab 会一直显示成「未激活」。同一线程里 SetFocus 到文本域就够打字了。
    // 只读（已完成的详情）也给文本域焦点：能选中复制，Esc 也和另外几种状态走同一条路。
    HWND target = g_edit ? g_edit : g_card;
    SetForegroundWindow(g_card);
    SetActiveWindow(g_card);
    SetFocus(target);
}

void ResetState(Mode mode, const std::wstring& title) {
    g_mode = mode;
    g_title = title;
    g_dark = !LightTheme();
    g_activated = false;
    g_hover = -1;
    g_press = -1;
    g_dragArmed = false;
    g_dragging = false;
    g_text.clear();
    g_done = false;
    g_readOnly = false;
    g_created = 0;
    g_textH = 0;
    g_editLines = 1;
    // 卡片尺寸回到默认：每次弹窗都是一张「刚开出来」的卡片，不记住上次拖成多大。
    // g_cardH 由调用方（ShowInput / ShowDetail）按内容定下来，然后再 EnsureBuffer。
    g_cardW = kInitCardW;
    g_cardH = 0;
    g_editH = 0;
    g_userSized = false;
    g_sizing = false;
    g_sizeW0 = 0;
    g_sizeH0 = 0;
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
    // 新建：初始高度就按「最小高度」= 除文本域以外那一圈固定高度 + 3 行文本域。
    // 传进来的 initial 本身是多行时，ShowInput 末尾的 RelayoutInput 再往下长。
    g_cardH = MinCardH();
    EnsureBuffer();
    if (!g_memDC) {
        // 建不出位图（极端内存压力）：别把调用方的收尾回调丢了
        if (onDone) onDone();
        return;
    }
    g_onOk = onOk;       // 拷一份，别 move：下面几条失败路径还要用原来的 onDone
    g_onDone = onDone;
    Layout();
    PlaceAndShow(owner);
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
    g_editLines = EditLineCountNow();
    RelayoutInput();   // 传进来的 initial 本身是多行的话，卡片跟着长高
    Paint();
    Present();
}

void ShowDetail(HWND owner, const std::wstring& text, bool done, long long created,
                std::function<void(const std::wstring&)> onSave,
                std::function<void()> onToggleDone, std::function<void()> onDone) {
    Close();
    EnsureGdiplus();
    EnsureCardClass();
    ResetState(Mode::Detail, L"\u5f85\u529e\u8be6\u60c5");   // 待办详情
    g_text = text;
    g_done = done;
    g_readOnly = done;   // 已完成的不许改内容（用户要求）
    g_created = created;
    // 正文多高由「文字实际多高」和「这块屏放得下多高」共同决定：
    // 先算出卡片除正文以外的固定高度（chrome），剩下的都留给正文。
    // 以前是硬编码 6 行 —— 输入还只有单行的时候够用，现在能写十几行就不够了。
    {
        const RECT wa = WorkAreaFor(owner);
        const int chrome = kShadow * 2 + kPadTop + kTitleH + kAfterTitle + kAfterEdit +
                           kBtnH + kPadBottom;
        const int room = (int)(wa.bottom - wa.top) - chrome - 32;   // 32 给屏幕上下留余量
        g_detailCap =
            std::clamp(room, kDetailLineH * kDetailMinLines, kDetailLineH * kDetailMaxLines);
    }
    g_textH = MeasureDetailHeight(g_text);   // 卡片高度要先知道正文多高
    // 正文也用文本域显示（未完成时可编辑），高度 = 量出来的正文高 + 文本域上下内边距，
    // 最少给 3 行那么高 —— 一条只有一行字的待办弹出来只有一条缝会很怪；
    // 最多到 g_detailCap（这块屏放得下的高度），再长就在文本域里自己滚。
    g_editH = std::clamp(g_textH + kEditPadY * 2, EditHeightFor(kDetailMinLines),
                         std::max(EditHeightFor(kDetailMinLines), g_detailCap));
    g_cardH = ChromeH() + g_editH;
    EnsureBuffer();
    if (!g_memDC) {
        if (onDone) onDone();
        return;
    }
    g_onSave = onSave;
    g_onToggle = onToggleDone;
    g_onDone = onDone;
    Layout();
    PlaceAndShow(owner);
    if (!g_card) {
        g_onSave = nullptr;
        g_onToggle = nullptr;
        g_onDone = nullptr;
        if (onDone) onDone();
        return;
    }
    if (!g_edit) {
        // 文本域没建出来（极端情况）：至少别让弹窗卡在那儿
        FinishDetail(false);
        return;
    }
    SetWindowTextW(g_edit, g_text.c_str());
    // 老数据可能比现在的限长还长：把限长抬到「现有长度」，否则用户在这条待办里
    // 一个字符都敲不进去（EM_SETLIMITTEXT 只限制用户输入，WM_SETTEXT 不受它约束）。
    if ((int)g_text.size() > kEditLimit)
        SendMessageW(g_edit, EM_SETLIMITTEXT, (WPARAM)(g_text.size() + 1), 0);
    // 不设选区：光标落在开头、视图停在顶部，看详情就该从第一行看起。
    g_editLines = EditLineCountNow();
    Paint();
    Present();
}

}  // namespace TodoDialog

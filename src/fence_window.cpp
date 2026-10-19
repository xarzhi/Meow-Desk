#include "fence_window.h"

#include <windowsx.h>
#include <objidl.h>
#include <propidl.h>
#include <shlwapi.h>
#include <gdiplus.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <vector>

#include "backdrop.h"
#include "desktop_surface.h"
#include "file_drop.h"
#include "icon_font.h"
#include "icon_render.h"
#include "screen_util.h"

#pragma comment(lib, "gdiplus.lib")
#pragma comment(lib, "shlwapi.lib")

using namespace Gdiplus;

// 拖动避让的统一数据源（定义见头文件里的注释）
std::function<std::vector<RECT>(const FenceWindow*)> FenceWindow::s_obstacleProvider;
// 抓磨砂底图前，把自家窗口从抓屏里排除掉
std::function<void(const RECT&, bool)> FenceWindow::s_captureExcluder;

void FenceWindow::SetObstacleProvider(
    std::function<std::vector<RECT>(const FenceWindow*)> fn) {
    s_obstacleProvider = std::move(fn);
}

void FenceWindow::SetCaptureExcluder(std::function<void(const RECT&, bool)> fn) {
    s_captureExcluder = std::move(fn);
}

namespace {

const wchar_t* kClass = L"MeowDeskFence";

const int kPad = 14;
const int kCellWDefault = 74;
// 图标列的额外内缩：两端对齐时首列不能真顶到 kPad 上，否则看起来「首列偏左、
// 末列偏右」。只在「图标 / 列 / 文字盒」定位上用，卡片内边距（kPad）不受影响。
const int kColInset = 10;

// 四条边/四角可以拖拽改大小的判定宽度
const int kEdgeMargin = 6;

// ---- 吸附（拖动位置 / 改尺寸共用）----
// 吸附距离（像素）：靠到这么近才贴上去。
// 注意「吸住之后拖不动」不是阈值的问题，那要归 dragRawX_/Y_（见 WM_MOUSEMOVE）——
// 拿吸附后的位置去累加位移，无论阈值多小都会粘死。
constexpr int kSnapPx = 10;

// 一维吸附：v 是盒子左边（上边），size 是宽（高）——
// 我的左边贴候选线，或者我的右边贴候选线。
int SnapAxis(int v, int size, const std::vector<int>& edges) {
    int bestV = v;
    int bestD = kSnapPx + 1;
    for (int e : edges) {
        int d = std::abs(v - e);
        if (d < bestD) {
            bestD = d;
            bestV = e;
        }
        d = std::abs(v + size - e);
        if (d < bestD) {
            bestD = d;
            bestV = e - size;
        }
    }
    return bestD <= kSnapPx ? bestV : v;
}

// 单条边的吸附（改尺寸用：只挪一条边）
int SnapEdge(int v, const std::vector<int>& edges) {
    int best = v;
    int bestD = kSnapPx + 1;
    for (int c : edges) {
        const int d = std::abs(v - c);
        if (d < bestD) {
            bestD = d;
            best = c;
        }
    }
    return bestD <= kSnapPx ? best : v;
}

// 候选吸附线：工作区的四条边 + 别的盒子的四条边。
// 只收「另一个方向上挨得比较近」的盒子 —— 隔着半个屏幕的盒子不该抢吸附。
void CollectSnapLines(const std::vector<RECT>& others, const RECT& wa, const RECT& want,
                      std::vector<int>& xs, std::vector<int>& ys) {
    xs.push_back((int)wa.left);
    xs.push_back((int)wa.right);
    ys.push_back((int)wa.top);
    ys.push_back((int)wa.bottom);
    for (const RECT& o : others) {
        // 横向吸附：纵向要有点关系（相交，或者首尾接得近）
        if ((int)o.bottom + kSnapPx >= (int)want.top &&
            (int)o.top - kSnapPx <= (int)want.bottom) {
            xs.push_back((int)o.left);
            xs.push_back((int)o.right);
        }
        if ((int)o.right + kSnapPx >= (int)want.left &&
            (int)o.left - kSnapPx <= (int)want.right) {
            ys.push_back((int)o.top);
            ys.push_back((int)o.bottom);
        }
    }
}

const UINT_PTR kAnimTimer = 2;
// 搜索「定位」的闪烁节奏：170ms 翻一次，4 相 = 亮→灭→亮→灭 = 正好两下
const UINT_PTR kFlashTimer = 3;
const int kFlashInterval = 170;
const int kFlashPhases = 4;
// 「鼠标移入切分组」现在是一移上去就切（用户嫌 200ms 延迟明显），
// 所以原来那个延迟定时器整个删掉了 —— 别再加回来。

void AddRoundRect(GraphicsPath& p, const RectF& r, float rad) {
    float d = rad * 2.0f;
    if (d > r.Width) d = r.Width;
    if (d > r.Height) d = r.Height;
    // 半径为 0 时 AddArc 画不出任何东西，整个路径会变空 → 背景完全不画。
    // 退化成普通矩形。
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

int Lighten(int v, double k) { return (int)(v + (255 - v) * k + 0.5); }

// 「查看」各档对应的格子宽度。和资源管理器一样：档位越大格子越宽（每行放得更少），
// 列表 / 详情直接变成宽格子，一行一个。
int CellWForMode(int m) {
    switch (m) {
    case 0: return 58;                    // 小图标
    case 2: return 92;                    // 大图标
    case 3: return 110;                   // 超大图标
    case 4: return 190;                   // 列表
    case 5: return 240;                   // 详情
    default: return kCellWDefault;        // 中等图标
    }
}

// 排序用的元数据：只有用户点「排序方式」时才批量读一次，平时不读。
struct SortInfo {
    std::wstring name;
    std::wstring ext;      // 小写扩展名，代表「类型」
    long long size = 0;
    FILETIME mtime{};
    bool isDir = false;
};

void FillSortInfo(DesktopIconView* e, SortInfo& s) {
    s.name = e->name;
    const size_t dot = e->name.find_last_of(L'.');
    if (dot != std::wstring::npos && dot + 1 < e->name.size())
        s.ext = e->name.substr(dot + 1);
    for (auto& c : s.ext)
        if (c >= L'A' && c <= L'Z') c = (wchar_t)(c - L'A' + L'a');

    WIN32_FILE_ATTRIBUTE_DATA fad{};
    if (!e->path.empty() &&
        GetFileAttributesExW(e->path.c_str(), GetFileExInfoStandard, &fad)) {
        s.isDir = (fad.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
        s.size = ((long long)fad.nFileSizeHigh << 32) | fad.nFileSizeLow;
        s.mtime = fad.ftLastWriteTime;
    }
}

bool IsLight(COLORREF c) {
    double l = 0.299 * GetRValue(c) + 0.587 * GetGValue(c) + 0.114 * GetBValue(c);
    return l > 140.0;
}

// ---- 头部标签栏的度量 ----
// 标签的宽度得按文字自己算（自绘标签栏没有控件帮忙排版）。量宽度用 GDI
// （每次鼠标移动都可能量一次，比建 Graphics 快），画文字仍然走 GDI+。
// 只有两个字号档位，各缓存一个 HFONT，用完不还原字体也能被下一轮复用。
HFONT TabMeasureFont(int px) {
    static HFONT cache[2] = {nullptr, nullptr};
    const int i = (px >= 13) ? 1 : 0;
    if (!cache[i]) {
        LOGFONTW lf{};
        lf.lfHeight = -(i ? 14 : 13);
        lf.lfWeight = FW_NORMAL;
        lf.lfQuality = CLEARTYPE_QUALITY;
        lstrcpynW(lf.lfFaceName, L"Microsoft YaHei UI", LF_FACESIZE);
        cache[i] = CreateFontIndirectW(&lf);
    }
    return cache[i];
}

int MeasureTabText(const std::wstring& s, int px) {
    if (s.empty()) return 0;
    HDC dc = GetDC(nullptr);
    if (!dc) return (int)s.size() * (px / 2);
    HGDIOBJ old = SelectObject(dc, TabMeasureFont(px));
    SIZE sz{};
    GetTextExtentPoint32W(dc, s.c_str(), (int)s.size(), &sz);
    SelectObject(dc, old);
    ReleaseDC(nullptr, dc);
    return sz.cx;
}

const int kTabPadX = 12;    // 标签左右内边距
const int kTabGap = 2;      // 相邻标签之间（小一点更像分段控件）
const int kTabLeft = 11;    // 第一个标签离卡片左边
const int kTabMinW = 40;
const int kTabMaxW = 152;
// 头部右侧被锁按钮（width-52±9）和箭头（最右 40px）占掉了，标签不能压过去
const int kTabRightReserve = 74;

int TabWidthFor(const std::wstring& name, int fontPx) {
    int w = MeasureTabText(name, fontPx) + kTabPadX * 2;
    if (w < kTabMinW) w = kTabMinW;
    if (w > kTabMaxW) w = kTabMaxW;
    return w;
}

ARGB WithAlpha(ARGB c, BYTE a) { return ((ARGB)a << 24) | (c & 0x00FFFFFFu); }

// 标签字体的行高：LabelDib 里 DrawTextW 是 DT_TOP，所以盒子高度给到行高、
// 再按 (行高 - 行高)/2 自然就垂直居中了。量一次缓存起来。
// 现在交给 icon_render 统一缓存 —— 桌面层算「两行标题」也用同一个值。
int LabelTextHeight() { return IconLabelLineHeight(); }

// 「类型」列：和资源管理器一样「PNG 图片」这种写法，靠扩展名 + 注册表里的类型描述。
// 注册表查询很贵（每项一次 RegQueryValue），所以只在部分常见扩展名上下探，
// 其余直接用「XXX 文件」——列表刷新时不至于卡顿。
std::wstring DescribeType(bool isDir, const std::wstring& ext) {
    if (isDir) return L"\u6587\u4ef6\u5939";   // 文件夹
    if (ext.empty()) return L"\u6587\u4ef6";   // 文件

    static const struct {
        const wchar_t* ext;
        const wchar_t* desc;
    } kKnown[] = {
        {L"exe", L"\u5e94\u7528\u7a0b\u5e8f"},          // 应用程序
        {L"lnk", L"\u5feb\u6377\u65b9\u5f0f"},          // 快捷方式
        {L"txt", L"\u6587\u672c\u6587\u6863"},          // 文本文档
        {L"md", L"\u6587\u672c\u6587\u6863"},           // 文本文档
        {L"log", L"\u6587\u672c\u6587\u6863"},          // 文本文档
        {L"png", L"PNG \u56fe\u7247"},                  // PNG 图片
        {L"jpg", L"JPG \u56fe\u7247"},                  // JPG 图片
        {L"jpeg", L"JPG \u56fe\u7247"},                 // JPG 图片
        {L"gif", L"GIF \u56fe\u7247"},                  // GIF 图片
        {L"bmp", L"BMP \u56fe\u7247"},                  // BMP 图片
        {L"webp", L"WEBP \u56fe\u7247"},                // WEBP 图片
        {L"svg", L"SVG \u56fe\u7247"},                  // SVG 图片
        {L"ico", L"\u56fe\u6807"},                      // 图标
        {L"pdf", L"PDF \u6587\u6863"},                  // PDF 文档
        {L"doc", L"Word \u6587\u6863"},                 // Word 文档
        {L"docx", L"Word \u6587\u6863"},                // Word 文档
        {L"xls", L"Excel \u5de5\u4f5c\u8868"},         // Excel 工作表
        {L"xlsx", L"Excel \u5de5\u4f5c\u8868"},        // Excel 工作表
        {L"ppt", L"PowerPoint \u5e7b\u706f\u7247"},     // PowerPoint 幻灯片
        {L"pptx", L"PowerPoint \u5e7b\u706f\u7247"},    // PowerPoint 幻灯片
        {L"zip", L"\u538b\u7f29\u6587\u4ef6\u5939"},    // 压缩文件夹
        {L"7z", L"\u538b\u7f29\u6587\u4ef6\u5939"},     // 压缩文件夹
        {L"rar", L"\u538b\u7f29\u6587\u4ef6\u5939"},    // 压缩文件夹
        {L"mp3", L"\u97f3\u9891\u6587\u4ef6"},          // 音频文件
        {L"flac", L"\u97f3\u9891\u6587\u4ef6"},         // 音频文件
        {L"wav", L"\u97f3\u9891\u6587\u4ef6"},          // 音频文件
        {L"mp4", L"\u89c6\u9891\u6587\u4ef6"},          // 视频文件
        {L"mkv", L"\u89c6\u9891\u6587\u4ef6"},          // 视频文件
        {L"avi", L"\u89c6\u9891\u6587\u4ef6"},          // 视频文件
        {L"mov", L"\u89c6\u9891\u6587\u4ef6"},          // 视频文件
        {L"js", L"JavaScript \u6587\u4ef6"},            // JavaScript 文件
        {L"ts", L"TypeScript \u6587\u4ef6"},            // TypeScript 文件
        {L"json", L"JSON \u6587\u4ef6"},                // JSON 文件
        {L"html", L"HTML \u6587\u6863"},                // HTML 文档
        {L"css", L"\u5c42\u53e0\u6837\u5f0f\u8868"},    // 层叠样式表
        {L"cpp", L"C++ \u6e90\u6587\u4ef6"},            // C++ 源文件
        {L"c", L"C \u6e90\u6587\u4ef6"},                // C 源文件
        {L"h", L"C \u5934\u6587\u4ef6"},                // C 头文件
        {L"py", L"Python \u6e90\u6587\u4ef6"},          // Python 源文件
        {L"db", L"\u6570\u636e\u5e93\u6587\u4ef6"},     // 数据库文件
        {L"ini", L"\u914d\u7f6e\u8bbe\u7f6e"},          // 配置设置
    };
    for (const auto& k : kKnown) {
        if (ext == k.ext) return k.desc;
    }

    std::wstring up = ext;
    for (auto& ch : up)
        if (ch >= L'a' && ch <= L'z') ch = (wchar_t)(ch - L'a' + L'A');
    return up + L" \u6587\u4ef6";   // XXX 文件
}

std::wstring FormatSize(long long n) {
    wchar_t buf[64]{};
    if (n < 1024)
        swprintf_s(buf, L"%lld B", n);
    else if (n < 1024LL * 1024)
        swprintf_s(buf, L"%.1f KB", n / 1024.0);
    else if (n < 1024LL * 1024 * 1024)
        swprintf_s(buf, L"%.1f MB", n / (1024.0 * 1024.0));
    else
        swprintf_s(buf, L"%.2f GB", n / (1024.0 * 1024.0 * 1024.0));
    return buf;
}

std::wstring FormatMTime(const FILETIME& ft) {
    FILETIME local{};
    SYSTEMTIME st{};
    if (!FileTimeToLocalFileTime(&ft, &local)) return std::wstring();
    if (!FileTimeToSystemTime(&local, &st)) return std::wstring();
    wchar_t buf[64]{};
    swprintf_s(buf, L"%04d/%02d/%02d %02d:%02d", st.wYear, st.wMonth, st.wDay, st.wHour,
               st.wMinute);
    return buf;
}

}  // namespace

LRESULT CALLBACK FenceWindow::WndProcStatic(HWND h, UINT m, WPARAM w, LPARAM l) {
    FenceWindow* self = reinterpret_cast<FenceWindow*>(GetWindowLongPtrW(h, GWLP_USERDATA));
    if (m == WM_NCCREATE) {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(l);
        self = reinterpret_cast<FenceWindow*>(cs->lpCreateParams);
        SetWindowLongPtrW(h, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    }
    if (self) return self->WndProc(h, m, w, l);
    return DefWindowProcW(h, m, w, l);
}

// 本盒子「当前分组」里的图标。
// 注意：所有以「第几个图标」为准的下标（命中、落点、flip 槽位）都是相对这个
// 列表的，所以这里必须带上 group 过滤 —— 否则切了分组图标还对不上号。
void FenceWindow::Collect(std::vector<DesktopIconView*>& out) const {
    out.clear();
    if (!all_) return;
    const int g = activeGroup();
    for (auto* e : *all_) {
        if (e && e->owner == index_ && e->group == g) out.push_back(e);
    }
}

int FenceWindow::CellCols() const {
    // 列表 / 详情：一行一个（和资源管理器一致），不受盒子宽度影响
    if (IsListView()) return 1;
    int n = (width_ - kPad * 2) / cellW_;
    if (n < 1) n = 1;
    return n;
}

// 两端对齐：以「图标宽度」为内容宽度做 space-between。
// 第 0 列图标左边缘 = kPad + kColInset，最后一列右边缘 = width_ - kPad - kColInset，
// 中间等步长；所以每一排满的时候两端都留出 kPad + kColInset 的空白，
// 不满的最后一排从左边起（比首列再向左一点点，但仍在卡片内），
// 而且列位置和上一排严丝合缝地对齐（步长相同）。
int FenceWindow::ColLeft() const { return kPad + kColInset; }
int FenceWindow::ColRight() const { return width_ - kPad - kColInset; }

int FenceWindow::ColCenterX(int c, int cols) const {
    const int iw = iconPx();
    const int lo = ColLeft() + iw / 2;
    const int hi = ColRight() - iw / 2;
    if (cols <= 1) return lo;
    if (c <= 0) return lo;
    if (c >= cols) return hi;
    return lo + (int)std::lround((double)(hi - lo) * c / (cols - 1));
}

void FenceWindow::ItemRect(int index, RECT& r) const {
    if (index < 0) {
        r = RECT{0, 0, 0, 0};
        return;
    }
    const int cols = CellCols();
    const int c = index % cols;
    const int row = index / cols;
    const int ch = cellH();
    const int top = HeadH() + kPad + row * ch - scrollY_;

    if (IsListView()) {
        r = RECT{kPad, top, width_ - kPad, top + ch};
        return;
    }
    // 命中区域：格子之间必须无缝铺满（否则图标之间那几像素的缝隙拖不进去），
    // 所以边界取相邻两列中心的中点。左右两端夹到卡片内边距。
    const int cx = ColCenterX(c, cols);
    const int left = (c == 0) ? kPad : (ColCenterX(c - 1, cols) + cx) / 2;
    const int right = (c == cols - 1) ? (width_ - kPad) : (cx + ColCenterX(c + 1, cols)) / 2;
    r = RECT{left, top, right, top + ch};
}

// 悬停 / 选中 / 落点高亮用的矩形：以图标中心为准、固定格子宽度，左右对称。
// 不能用 ItemRect —— 首列的左边界是 kPad，比「中心 ± 半格」更靠左，
// 直接拿去画高亮会偏到图标右边去（图标看着就不在高亮框中间）。
RECT FenceWindow::ItemHighlightRect(int index) const {
    const int ch = cellH();
    if (index < 0) return RECT{0, 0, 0, 0};
    const int cols = CellCols();
    if (IsListView()) {
        RECT r{};
        ItemRect(index, r);
        return r;
    }
    const int row = index / cols;
    const int c = index % cols;
    const int top = HeadH() + kPad + row * ch - scrollY_;
    const int cx = ColCenterX(c, cols);
    const int half = cellW_ / 2;
    int l = cx - half;
    int rr = cx + half;
    if (l < 2) l = 2;
    if (rr > width_ - 2) rr = width_ - 2;
    return RECT{l, top, rr, top + ch};
}

void FenceWindow::ItemLabelBox(int index, int& left, int& w) const {
    const int cols = CellCols();
    const int c = (index < 0) ? 0 : (index % cols);
    const int usable = width_ - kPad * 2;
    if (cols <= 1) {
        w = std::min(cellW_ - 8, usable);
    } else {
        // 步长要扣掉 6px，相邻两列的文字盒才不会贴到一起
        const double step = cols > 1 ? (double)(ColCenterX(1, cols) - ColCenterX(0, cols)) : 0.0;
        w = std::min(cellW_ - 8, std::max(24, (int)step - 6));
    }
    const int cx = ColCenterX(c, cols);
    left = cx - w / 2;
    // 文字盒可以比图标列宽一点（夹到卡片内边距即可），但首末列不能把图标顶偏
    if (left < kPad) left = kPad;
    if (left + w > width_ - kPad) left = width_ - kPad - w;
}

int FenceWindow::minW() const {
    // 列表 / 详情是一行一个，右边的列会按宽度逐级隐藏，所以可以收得很窄
    // （留出「图标 + 名称」的最低宽度即可）
    if (IsListView()) return kPad * 2 + 132;
    return kPad * 2 + cellW_;   // 至少放得下一列
}

int FenceWindow::minH() const {
    return HeadH() + 64;        // 头部 + 一行图标的空间
}

// 头部右侧的锁按钮矩形（客户区）。箭头占最右边 40px，锁放在它左边一格。
RECT FenceWindow::LockRect() const {
    const int cy = HeadH() / 2;
    const int cx = width_ - 52;
    return RECT{cx - 9, cy - 9, cx + 9, cy + 9};
}

void FenceWindow::SetLocked(bool on) {
    if (!cfg_) return;
    const bool was = cfg_->locked != 0;
    cfg_->locked = on ? 1 : 0;
    if (on && !was) {
        // 正在拖动 / 改尺寸时被锁上（理论上有），立刻收掉手势，
        // 否则鼠标还按着，盒子会继续跟着走 —— 看着就像「锁不住」。
        dragging_ = false;
        resizing_ = false;
        resizeEdge_ = Edge::None;
    }
    Redraw();
    if (onStateChanged) onStateChanged();   // 存配置 + 存布局
}

// 命中四条边/四个角（客户区坐标）。收起状态下不参与，避免和「拖头部移动」打架。
FenceWindow::Edge FenceWindow::HitEdge(POINT pt) const {
    if (collapsed_) return Edge::None;
    if (locked()) return Edge::None;   // 锁定的盒子不能改尺寸
    const int m = kEdgeMargin;
    const bool left = pt.x < m;
    const bool right = pt.x >= width_ - m;
    const bool top = pt.y < m;
    const bool bottom = pt.y >= curH_ - m;
    if (!left && !right && !top && !bottom) return Edge::None;
    // 角落优先
    if (left && top) return Edge::TL;
    if (right && top) return Edge::TR;
    if (left && bottom) return Edge::BL;
    if (right && bottom) return Edge::BR;
    if (left) return Edge::L;
    if (right) return Edge::R;
    if (top) return Edge::T;
    return Edge::B;
}

// 处理 WM_LBUTTONDOWN / WM_LBUTTONDBLCLK（消息参数语义一致，重复一份现场初始化）
void FenceWindow::BeginPress(HWND h, POINT pt) {
    pressPt_ = pt;
    // 拖动必须用屏幕坐标：窗口一动客户区坐标基准就跟着动
    GetCursorPos(&pressScreenPt_);
    GetWindowRect(h, &dragStartRect_);
    dragLastX_ = dragStartRect_.left;
    dragLastY_ = dragStartRect_.top;
    dragRawX_ = dragLastX_;
    dragRawY_ = dragLastY_;
    dragging_ = false;
    itemDragging_ = false;
    pressCaptured_ = false;
    resizing_ = false;
    resizeEdge_ = Edge::None;
    pressItem_ = -1;
    pressArrow_ = false;
    pressLock_ = false;
    pressTab_ = -1;
    pressHeader_ = false;
    pressBody_ = false;
    marquee_ = false;
    // 多选的修饰键必须在「按下」这一刻取：松手时用户可能已经松开 Ctrl 了
    pressCtrl_ = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
    pressShift_ = (GetKeyState(VK_SHIFT) & 0x8000) != 0;

    // 锁按钮优先级最高：它压在头部右侧，点在它上面不能算「拖头部」。
    // 收起状态（4px 把手）里头部内容一个都不画，所以锁 / 标签 / 箭头都不参与命中，
    // 整条把手就是「拖盒子」的抓手动。
    if (showTitle_ && !collapsed_ && pt.y < HeadH()) {
        const RECT lr = LockRect();
        if (pt.x >= lr.left && pt.x < lr.right && pt.y >= lr.top && pt.y < lr.bottom) {
            pressLock_ = true;
            SetCapture(h);
            return;
        }
        // 点在标签上：先记下来，松手时（没拖动过）再切分组。
        // 注意不能在这里直接切 —— 用户可能只是想按住头部拖盒子。
        pressTab_ = HitTab(pt);
    }

    // 先判边缘：边缘优先于头部拖动/图标
    const Edge e = HitEdge(pt);
    if (e != Edge::None) {
        resizing_ = true;
        resizeCaptured_ = false;   // 第一次真正拉动时才抓底图（见 ApplyResize）
        resizeEdge_ = e;
        resizeStartRect_ = dragStartRect_;
        resizeStartW_ = width_;
        resizeStartH_ = height_;
        // 底图不在这里重抓：只是「按下」还没动，提前抓会让单击边缘也卡一下。
        // 真正开始改尺寸时 ApplyResize → EnsureBackdropCover 会补一圈。
        SetCapture(h);
        return;
    }

    if (pt.y < HeadH() && showTitle_ && !collapsed_ && pt.x > width_ - 40) {
        pressArrow_ = true;
    } else if (pt.y < HeadH()) {
        // 锁定后头部不再能拖动（不然「锁住位置」就成了空话）
        pressHeader_ = !locked();
    } else if (!collapsed_) {
        pressBody_ = true;
        pressItem_ = HitItem(pt);
        // 空白处拖 = 框选；按住 Ctrl 拖框是「追加选中」
        marqueeAdditive_ = pressCtrl_;
    }

    // 按在「能拖起盒子」的位置上：趁盒子还没动，同步抓一圈带余量的磨砂底图。
    //
    // 放在这里而不是「第一次移动」时抓，是为了让拖动的第一帧就跟手：
    // 抓屏 + 模糊是十几毫秒，压在移动的第一帧上就是一次可见的启动停顿
    // （用户说的「拖起来黏」）。按下时盒子纹丝不动，这点时间根本看不出来。
    // 单击头部（只是想切焦点）也照样会抓一次，十几毫秒，感觉不到。
    if ((pressHeader_ || pressArrow_) && cfg_ && cfg_->acrylic) {
        RefreshBackdrop(128);
        pressCaptured_ = true;
    }

    SetCapture(h);
}

void FenceWindow::ApplyResize(POINT screenPt) {
    if (!resizing_ || !hwnd_) return;
    const int dx = screenPt.x - pressScreenPt_.x;
    const int dy = screenPt.y - pressScreenPt_.y;

    // 第一次真的开始改尺寸：趁盒子还没变，同步抓一圈带余量的底图。
    // 之后整个拉伸过程都不再抓屏（抓屏会抢 GDI 锁，边拉边抓必卡）。
    if (!resizeCaptured_) {
        resizeCaptured_ = true;
        if (cfg_ && cfg_->acrylic) RefreshBackdrop(128);
    }

    int l = resizeStartRect_.left, t = resizeStartRect_.top;
    int r = resizeStartRect_.right, b = resizeStartRect_.bottom;
    const Edge e = resizeEdge_;

    if (e == Edge::L || e == Edge::TL || e == Edge::BL) l += dx;
    if (e == Edge::R || e == Edge::TR || e == Edge::BR) r += dx;
    if (e == Edge::T || e == Edge::TL || e == Edge::TR) t += dy;
    if (e == Edge::B || e == Edge::BL || e == Edge::BR) b += dy;

    const bool moveL = (e == Edge::L || e == Edge::TL || e == Edge::BL);
    const bool moveR = (e == Edge::R || e == Edge::TR || e == Edge::BR);
    const bool moveT = (e == Edge::T || e == Edge::TL || e == Edge::TR);
    const bool moveB = (e == Edge::B || e == Edge::BL || e == Edge::BR);

    // 边缘吸附：改尺寸时也要能贴上别的盒子 / 屏幕边。
    // 只吸「正在拖的那条边」，固定不动的那几条边不参与。
    // 「自动吸附」关掉就整段跳过 —— 用户拖到哪就是哪，绝不偷偷挪坐标。
    if (g_config.autoSnap) {
        std::vector<RECT> others;
        if (s_obstacleProvider) others = s_obstacleProvider(this);
        std::vector<int> xs, ys;
        const RECT self{l, t, r, b};
        // 吸附线不收自己，但 s_obstacleProvider 已经排除自己了
        CollectSnapLines(others, ScreenUtil::WorkAreaForRect(self), self, xs, ys);
        if (moveL) l = SnapEdge(l, xs);
        if (moveR) r = SnapEdge(r, xs);
        if (moveT) t = SnapEdge(t, ys);
        if (moveB) b = SnapEdge(b, ys);
    }

    // 最小尺寸（从左/上拖时调的是 left/top）
    if (r - l < minW()) {
        if (moveL) l = r - minW();
        else r = l + minW();
    }
    if (b - t < minH()) {
        if (moveT) t = b - minH();
        else b = t + minH();
    }

    // 不出屏幕（多屏：按「矩形压在哪块屏上」取工作区，跨屏时是并集）
    {
        const RECT wa = ScreenUtil::WorkAreaForRect(RECT{l, t, r, b});
        if (moveL && l < (int)wa.left) l = (int)wa.left;
        if (moveT && t < (int)wa.top) t = (int)wa.top;
        if (moveR && r > (int)wa.right) r = (int)wa.right;
        if (moveB && b > (int)wa.bottom) b = (int)wa.bottom;
    }
    if (r - l < minW()) r = l + minW();
    if (b - t < minH()) b = t + minH();

    const int nw = r - l;
    const int nh = b - t;
    if (nw == width_ && nh == height_ && l == dragLastX_ && t == dragLastY_) return;

    width_ = nw;
    height_ = nh;
    curH_ = nh;
    targetH_ = nh;
    dragLastX_ = l;
    dragLastY_ = t;
    SetWindowPos(hwnd_, nullptr, l, t, nw, nh, SWP_NOZORDER | SWP_NOACTIVATE);
    // 尺寸变了，图标可能换行/换列；列宽也变了，标签和详情列缓存都得重做
    ClearSelection();
    hot_ = -1;
    if (nw != 0) {
        labels_.clear();
        labelKeys_.clear();
        ClearDetailCache();
    }
    StopFlip();
    // 改尺寸时盒子本身在长/缩，磨砂底图那一圈要跟着盖得住 ——
    // 以前尺寸一变就对不上（要求 backdropW_ == bufW_），整块退回纯半透明，
    // 看起来就是「拖大小的时候盒子变透明了」。
    if (cfg_ && cfg_->acrylic) EnsureBackdropCover();
    Redraw();
}

// ---------------------------------------------------------------------------
// 查看 / 排序（右键菜单驱动）
// ---------------------------------------------------------------------------
int FenceWindow::cellH() const {
    // 标题区固定按「两行」预留：平时只画一行（超出省略），单击选中后展开成两行。
    // 这样展开时不用改格子高度，图标不会因为「点了一下」就重新排位。
    // 行高从标题字体量出来，不能写死 —— 换系统字号 / 高 DPI 会露半个字。
    const int twoLineH = 4 + iconPx() + 2 + LabelTextHeight() * 2;
    switch (viewMode()) {
    case 0: return showLabels_ ? std::max(64, twoLineH) : 50;     // 小图标
    case 2: return showLabels_ ? std::max(102, twoLineH) : 82;    // 大图标
    case 3: return showLabels_ ? std::max(126, twoLineH) : 102;   // 超大图标
    case 4: return showLabels_ ? 52 : 44;     // 列表（图标 32px，一行一个）
    case 5: return 44;                        // 详情：固定行高，名称在右侧列里
    default: return showLabels_ ? std::max(84, twoLineH) : kCellHCompact;   // 中等图标
    }
}

int FenceWindow::viewMode() const { return cfg_ ? cfg_->viewMode : 1; }
int FenceWindow::sortKey() const { return cfg_ ? cfg_->sortKey : 4; }
bool FenceWindow::sortAsc() const { return cfg_ ? cfg_->sortAsc != 0 : true; }

// 「查看」档位对应的图标像素尺寸。
// 图标位图是每个图标一份，盒子里的数量有限（十几个），按档位重载是划算的：
// 真的放大图标才有「大图标」的样子，只把格子撑大看起来只是间距变松。
int FenceWindow::iconPx() const {
    switch (viewMode()) {
    case 0: return 32;    // 小
    case 2: return 64;    // 大
    case 3: return 96;    // 超大
    case 4: return 32;    // 列表
    case 5: return 32;    // 详情（行高 44，32px 图标上下各留 6px）
    default: return 48;   // 中等
    }
}

void FenceWindow::SetViewMode(int m) {
    if (m < 0 || m > 5) return;
    if (cfg_) cfg_->viewMode = m;

    const int w = CellWForMode(m);
    if (w != cellW_) {
        cellW_ = w;
        // 格子宽了/窄了，标签可用的宽度也变了，缓存得重做
        labels_.clear();
        labelKeys_.clear();
    }
    ClearDetailCache();
    StopFlip();
    if (m == 5) BuildDetailInfo();
    else detailInfo_.clear();
    ClearSelection();
    hot_ = -1;
    Redraw();
    if (onStateChanged) onStateChanged();
}

// 详情行：读一次文件属性，把「日期 / 类型 / 大小」三列算好缓存。
// 盒子里的图标一般十几个，一次 IO 完全可以接受；渲染时不再碰磁盘。
void FenceWindow::BuildDetailInfo() {
    detailInfo_.clear();
    if (!all_) return;
    const int g = activeGroup();
    for (auto* e : *all_) {
        if (!e || e->owner != index_ || e->group != g) continue;

        DetailInfo d;
        WIN32_FILE_ATTRIBUTE_DATA fad{};
        bool isDir = false;
        const bool hasAttr =
            !e->path.empty() &&
            GetFileAttributesExW(e->path.c_str(), GetFileExInfoStandard, &fad) != FALSE;
        if (hasAttr) {
            isDir = (fad.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
            d.date = FormatMTime(fad.ftLastWriteTime);
            if (!isDir) d.size = FormatSize(((long long)fad.nFileSizeHigh << 32) |
                                            fad.nFileSizeLow);
        }

        // 类型靠扩展名（没有属性时也照扩展名猜，至少不是空白）
        std::wstring ext;
        const size_t dot = e->name.find_last_of(L'.');
        if (dot != std::wstring::npos && dot + 1 < e->name.size())
            ext = e->name.substr(dot + 1);
        for (auto& c : ext)
            if (c >= L'A' && c <= L'Z') c = (wchar_t)(c - L'A' + L'a');
        d.type = DescribeType(isDir, ext);

        detailInfo_[e] = std::move(d);
    }
    ClearDetailCache();
}

// 详情列缓存：同一段文字 + 同一排版盒子只栅格化一次。
// 排版盒子是「列宽 x 行高」，盒子改宽或换档位时会变，所以外面会主动清缓存。
LabelDib* FenceWindow::DetailText(const std::wstring& text, int boxW, int boxH, int glow) {
    if (text.empty() || boxW <= 0 || boxH <= 0) return nullptr;
    std::wstring key = text + L"\x1", s;
    s = std::to_wstring(boxW) + L"x" + std::to_wstring(boxH) + L"g" + std::to_wstring(glow);
    key += s;

    auto it = detailText_.find(key);
    if (it != detailText_.end()) return it->second.dib.valid() ? &it->second.dib : nullptr;

    DetailTextEntry en;
    en.key = key;
    // 和桌面/盒内图标标题完全同一套栅格化：GDI + 灰度抗锯齿 + 柔和黑光晕。
    // 之前这里走 GDI+ 的 DrawString（11.5px 非整数 + 8 圈描边），笔画被反复叠加发灰，
    // 就是用户看到的「文件大小字体有点模糊」。
    if (!en.dib.Build(text, IconLabelFont(), boxW, boxH, glow, RGB(255, 255, 255), 1)) {
        detailText_[key] = std::move(en);   // 缓存失败结果，避免每帧重试
        return nullptr;
    }
    auto r = detailText_.insert_or_assign(key, std::move(en));
    return &r.first->second.dib;
}

void FenceWindow::ClearDetailCache() { detailText_.clear(); }

// 详情列几何：图标在左，名称占剩余空间，右侧依次 修改日期 / 类型 / 大小（右对齐）。
// 盒子变窄时按用户要求的顺序隐藏：大小 → 类型 → 修改日期；名称至少留 80px。
FenceWindow::DetailCols FenceWindow::DetailLayoutFor(int cellLeft, int cellWidth) const {
    DetailCols r;
    const int iconW = iconPx();
    r.iconX = cellLeft + 6;
    const int nameStart = cellLeft + 6 + iconW + 10;
    const int right = cellLeft + cellWidth;
    const int avail = right - nameStart;
    if (avail <= 0) {
        r.nameX = cellLeft;
        r.nameW = cellWidth;
        return r;
    }

    const int kGap = 8;
    const int kMinName = 80;
    const int kDateW = 110;   // "2026/10/18 14:30" 刚好放得下
    const int kTypeW = 84;
    const int kSizeW = 66;

    int need = 0;
    bool showDate = true, showType = true, showSize = true;
    auto total = [&]() {
        int n = 0;
        if (showDate) n += kDateW + kGap;
        if (showType) n += kTypeW + kGap;
        if (showSize) n += kSizeW + kGap;
        return n;
    };
    need = total();
    if (avail - need < kMinName) { showSize = false; need = total(); }
    if (avail - need < kMinName) { showType = false; need = total(); }
    if (avail - need < kMinName) { showDate = false; need = total(); }

    r.showDate = showDate;
    r.showType = showType;
    r.showSize = showSize;
    r.nameX = nameStart;
    r.nameW = std::max(0, avail - need);
    // 从右往左排：大小最靠右，其次类型，最左是日期
    int x = right;
    if (showSize) {
        x -= kSizeW;
        r.sizeX = x;
        r.sizeW = kSizeW;
        x -= kGap;
    }
    if (showType) {
        x -= kTypeW;
        r.typeX = x;
        r.typeW = kTypeW;
        x -= kGap;
    }
    if (showDate) {
        x -= kDateW;
        r.dateX = x;
        r.dateW = kDateW;
        x -= kGap;
    }
    return r;
}

void FenceWindow::SetSort(int key, bool asc) {
    if (key < 0 || key > 4) return;

    // 排序前的槽位先记下来，排完才能知道每个图标从哪飞到哪（flip 动画用）
    std::vector<DesktopIconView*> before;
    CollectVisible(before);
    std::unordered_map<DesktopIconView*, int> fromSlot;
    fromSlot.reserve(before.size());
    for (size_t i = 0; i < before.size(); ++i) fromSlot[before[i]] = (int)i;

    if (cfg_) {
        cfg_->sortKey = key;
        cfg_->sortAsc = asc ? 1 : 0;
    }
    SortNow();

    std::vector<DesktopIconView*> after;
    CollectVisible(after);
    StartFlip(fromSlot, after);

    ClearSelection();
    hot_ = -1;
    Redraw();
    if (onStateChanged) onStateChanged();
}

// ---- 排序 flip 动画 -------------------------------------------------------
// 做法：每张图标绕「竖轴」翻过去 —— 前半程在原槽位横向压扁到 0（cos: 1→0），
// 后半程在新槽位上从 0 张开（sin: 0→1）。文案同时按 scale 淡出/淡入。
// 全部图标同时翻，看上去就是整片图标「啪」地重排，比直接瞬移可读得多。
void FenceWindow::StartFlip(const std::unordered_map<DesktopIconView*, int>& fromSlot,
                            const std::vector<DesktopIconView*>& after) {
    StopFlip();
    // 两个开关都得关才算「不要动画」：sortAnim 管排序过渡，flipAnim 管拖放插入。
    // 设置页里它们是一个开关，但配置分开存（见 config.h）。
    if (!g_config.sortAnim && !g_config.flipAnim) return;
    if (collapsed_) return;
    if (after.size() < 2) return;

    bool moved = false;
    flipNodes_.reserve(after.size());
    for (size_t i = 0; i < after.size(); ++i) {
        auto it = fromSlot.find(after[i]);
        if (it == fromSlot.end()) continue;
        FlipNode n;
        n.e = after[i];
        n.from = it->second;
        n.to = (int)i;
        if (n.from != n.to) moved = true;
        flipNodes_[after[i]] = n;
    }
    if (!moved) {
        flipNodes_.clear();
        return;
    }
    flipActive_ = true;
    flipT0_ = GetTickCount();
    if (hwnd_) SetTimer(hwnd_, kAnimTimer, 16, nullptr);
}

void FenceWindow::StopFlip() {
    flipActive_ = false;
    flipNodes_.clear();
}

// 拖拽落位用：记下「现在」每个图标占第几格。改完列表之后调 PlayPendingFlip()，
// 走的就是和「改排序」同一套 StartFlip —— 插入到中间时，后面的图标会平滑地
// 往后让一格，而不是「啪」一下整体跳位。
void FenceWindow::CaptureSlots() {
    pendingSlots_.clear();
    std::vector<DesktopIconView*> mine;
    CollectVisible(mine);
    pendingSlots_.reserve(mine.size() + 4);
    for (size_t i = 0; i < mine.size(); ++i) pendingSlots_[mine[i]] = (int)i;
}

void FenceWindow::PlayPendingFlip() {
    if (pendingSlots_.empty()) return;
    std::vector<DesktopIconView*> after;
    CollectVisible(after);
    auto from = std::move(pendingSlots_);
    pendingSlots_.clear();
    StartFlip(from, after);
    Redraw();
}

float FenceWindow::FlipProgress() const {
    if (!flipActive_ || flipDur_ <= 0) return 1.0f;
    const DWORD el = GetTickCount() - flipT0_;
    if (el >= (DWORD)flipDur_) return 1.0f;
    return (float)el / (float)flipDur_;
}

void FenceWindow::SetTitle(const std::wstring& t) {
    if (!cfg_) return;
    cfg_->title = t;
    // 有分组时第一个标签就是盒子标题，改名要跟着走
    if (!cfg_->groups.empty()) cfg_->groups[0] = t;
    Redraw();
    if (onStateChanged) onStateChanged();
}

std::wstring FenceWindow::title() const { return cfg_ ? cfg_->title : std::wstring(); }

// ---------------------------------------------------------------------------
// 分组（头部标签栏）
// ---------------------------------------------------------------------------
std::wstring FenceWindow::GroupName(int i) const {
    if (!cfg_) return std::wstring();
    if (cfg_->groups.empty()) return (i == 0) ? cfg_->title : std::wstring();
    if (i < 0 || i >= (int)cfg_->groups.size()) return std::wstring();
    return cfg_->groups[i];
}

int FenceWindow::activeGroup() const {
    if (!cfg_) return 0;
    const int n = groupCount();
    if (cfg_->activeGroup < 0) return 0;
    if (cfg_->activeGroup >= n) return n - 1;
    return cfg_->activeGroup;
}

// 切分组：换一批图标，所以所有「指向某个图标」的下标都要作废
void FenceWindow::SetActiveGroup(int g) {
    if (!cfg_) return;
    const int n = groupCount();
    if (g < 0) g = 0;
    if (g >= n) g = n - 1;
    // 先把滑块现在的位置记下来（改 activeGroup 之后 TabRects 就是新布局了）
    const RECT pillBefore = TabPillRect();
    cfg_->activeGroup = g;

    hot_ = -1;
    ClearSelection();
    pressItem_ = -1;
    pressTab_ = -1;
    marquee_ = false;
    drop_ = DropHint{};
    scrollY_ = 0;   // 换了一批图标，滚动位置从头开始
    // 拖拽过程中（把图标划过标签栏）也会走到这里：这时只能作废「下标」，
    // 绝不能清 itemDragging_ / dragItems_ —— 否则一次拖动会被打断成「松手什么都不发生」。
    if (!itemDragging_) dragged_ = nullptr;
    StopFlip();
    InvalidateDetail();
    KickTabPill(pillBefore);

    Redraw();
    if (onStateChanged) onStateChanged();   // 存配置（activeGroup 要落盘）
}

// 滑块补间：from 是切换前滑块所在的位置（可能是上一次动画的中间态），
// 终点是当前 activeGroup 对应的标签矩形。
void FenceWindow::KickTabPill(const RECT& from) {
    const std::vector<RECT> tabs = TabRects();
    const int act = activeGroup();
    if (act < 0 || act >= (int)tabs.size()) {
        tabPillAnim_ = false;
        return;
    }
    const RECT to = tabs[act];
    RECT src = from;
    if (src.right <= src.left) src = to;   // 还没有滑块（第一次出现标签）→ 直接就位
    tabPillFrom_ = src;
    tabPillTo_ = to;
    if (src.left == to.left && src.top == to.top && src.right == to.right &&
        src.bottom == to.bottom) {
        tabPillAnim_ = false;
        return;
    }
    tabPillT0_ = GetTickCount();
    tabPillAnim_ = true;
    if (hwnd_) SetTimer(hwnd_, kAnimTimer, 16, nullptr);
}

// 滑块当前应该画在哪：动画中按 easeInOutCubic 在 from/to 之间取值
RECT FenceWindow::TabPillRect() const {
    const std::vector<RECT> tabs = TabRects();
    const int act = activeGroup();
    RECT target{};
    if (act >= 0 && act < (int)tabs.size()) target = tabs[act];
    if (!tabPillAnim_ || tabPillDur_ <= 0) return target;

    const DWORD el = GetTickCount() - tabPillT0_;
    if (el >= (DWORD)tabPillDur_) return target;
    const float p = (float)el / (float)tabPillDur_;
    // 和 flip 动画同一套缓动：起步收尾都柔，中间快
    const float t = (p < 0.5f) ? 4.0f * p * p * p
                              : 1.0f - std::pow(-2.0f * p + 2.0f, 3.0f) / 2.0f;
    auto lerpI = [&](int a, int b) { return a + (int)std::lround((double)(b - a) * t); };
    return RECT{lerpI(tabPillFrom_.left, tabPillTo_.left),
                lerpI(tabPillFrom_.top, tabPillTo_.top),
                lerpI(tabPillFrom_.right, tabPillTo_.right),
                lerpI(tabPillFrom_.bottom, tabPillTo_.bottom)};
}

int FenceWindow::AddGroup(const std::wstring& name) {
    if (!cfg_) return -1;
    // 第一次创建分组时把盒子标题补成第 0 条：这样「标题」和「分组」是同一套下标
    if (cfg_->groups.empty()) cfg_->groups.push_back(cfg_->title);

    std::wstring n = name;
    if (n.empty()) n = L"\u5206\u7ec4 " + std::to_wstring(cfg_->groups.size());
    cfg_->groups.push_back(n);

    const int idx = (int)cfg_->groups.size() - 1;
    SetActiveGroup(idx);
    return idx;
}

void FenceWindow::RemoveActiveGroup() {
    if (!cfg_) return;
    const int g = activeGroup();
    // 第 0 个标签就是盒子标题本身，删掉它盒子就没名字了
    if (g <= 0 || cfg_->groups.size() <= 1) return;

    const RECT pillBefore = TabPillRect();
    cfg_->groups.erase(cfg_->groups.begin() + g);
    // 这个分组里的图标退回第 0 组（盒子标题页），后面分组的图标下标各减一 ——
    // 不这么做的话它们会指向一个不存在的分组，变成「怎么也看不见的图标」。
    if (all_) {
        for (auto* e : *all_) {
            if (!e || e->owner != index_) continue;
            if (e->group == g) e->group = 0;
            else if (e->group > g) --e->group;
        }
    }

    const int n = (int)cfg_->groups.size();
    cfg_->activeGroup = std::min(g, n - 1);
    hot_ = -1;
    ClearSelection();
    drop_ = DropHint{};
    StopFlip();
    InvalidateDetail();
    KickTabPill(pillBefore);
    Redraw();
    if (onStateChanged) onStateChanged();
}

// 改某个标签的名字。下标 0 就是盒子标题（两边同步），没有分组时等同于改标题。
void FenceWindow::SetGroupName(int i, const std::wstring& n) {
    if (!cfg_ || n.empty()) return;
    if (cfg_->groups.empty() || i <= 0) {
        SetTitle(n);
        return;
    }
    if (i >= (int)cfg_->groups.size()) return;
    cfg_->groups[i] = n;
    Redraw();
    if (onStateChanged) onStateChanged();
}

// 标签的排版：从左边 12px 开始一个个排，放不下就截断（锁按钮和箭头占右边）。
std::vector<RECT> FenceWindow::TabRects() const {
    std::vector<RECT> out;
    const int n = groupCount();
    if (n <= 0 || !showTitle_) return out;
    const int hh = HeadH();
    if (hh < 16) return out;

    const int fontPx = (headerH_ >= 40) ? 14 : 13;
    const int th = std::min(hh - 8, 30);
    const int top = (hh - th) / 2;
    const int rightLimit = width_ - kTabRightReserve;

    int x = kTabLeft;
    for (int i = 0; i < n; ++i) {
        int w = TabWidthFor(GroupName(i), fontPx);
        if (x + w > rightLimit) {
            if (i > 0) break;
            w = std::max(kTabMinW, rightLimit - x);   // 第一个再挤也要留一块
        }
        out.push_back(RECT{x, top, x + w, top + th});
        x += w + kTabGap;
    }
    return out;
}

int FenceWindow::HitTab(POINT clientPt) const {
    if (!showTitle_ || !hasTabs()) return -1;
    const auto rects = TabRects();
    for (size_t i = 0; i < rects.size(); ++i) {
        const RECT& r = rects[i];
        if (clientPt.x >= r.left && clientPt.x < r.right && clientPt.y >= r.top &&
            clientPt.y < r.bottom)
            return (int)i;
    }
    return -1;
}

// ---------------------------------------------------------------------------
// 盒子标题 / 分组标签的屏幕矩形（盒子/分组改名已改成弹窗，这里现在只做几何参考）
// ---------------------------------------------------------------------------
RECT FenceWindow::RenameRectScreen(int gi) const {
    RECT none{0, 0, 0, 0};
    if (!hwnd_ || !showTitle_) return none;
    const int hh = HeadH();
    if (hh < 16) return none;

    RECT r{};
    if (hasTabs()) {
        const auto tabs = TabRects();
        if (gi < 0 || gi >= (int)tabs.size()) return none;
        r = tabs[gi];
    } else {
        if (gi > 0) return none;
        // 和 Paint 里「没有分组 → 标题单独画一行」那一支对齐：
        //   RectF(16, 0, width_ - 56, headH)，纵向居中
        const int th = std::min(hh - 8, 30);
        const int top = (hh - th) / 2;
        r = RECT{16, top, width_ - 56, top + th};
    }
    MapWindowPoints(hwnd_, nullptr, reinterpret_cast<LPPOINT>(&r), 2);
    return r;
}

bool FenceWindow::ItemLabelScreenRect(const DesktopIconView* e, RECT& out) const {
    if (!e || !hwnd_ || collapsed_) return false;
    // 下标必须按「实际参与布局的那批」算 —— 和 Paint / HitItem 用一个口径
    std::vector<DesktopIconView*> mine;
    CollectVisible(mine);
    int idx = -1;
    for (size_t i = 0; i < mine.size(); ++i) {
        if (mine[i] == e) {
            idx = (int)i;
            break;
        }
    }
    if (idx < 0) return false;

    int ix = 0, iy = 0;
    if (!ItemOrigin(idx, ix, iy)) return false;   // 滚出视野 / 放不下

    const int ch = cellH();
    const int lineH = LabelTextHeight();
    const int iconW = e->bmp.valid() ? e->bmp.width : 32;
    const int iconH = e->bmp.valid() ? e->bmp.height : 32;
    const int twoLine = lineH * 2;

    RECT r{};
    if (viewMode() == 5) {
        // 详情模式：名称是表格里的一列（左对齐、单行），不是图标下方那块标签
        const DetailCols dl = DetailLayoutFor(kPad, width_ - kPad * 2);
        if (dl.nameW <= 0) return false;
        const int top = iy + (ch - lineH) / 2;
        r = RECT{dl.nameX, top, dl.nameX + dl.nameW, top + lineH};
    } else if (IsListView()) {
        // 列表：图标靠左 8px，标签在它右边（几何取自 Paint 的 listMode 分支）
        const int iconX = ix + 8;
        const int labW = std::max(24, (width_ - kPad * 2) - (iconX - ix) - iconW - 8);
        int th = iconH;
        if (th < twoLine) th = std::min(twoLine, ch - 2);
        const int left = iconX + iconW + 8;
        r = RECT{left, iy, left + labW, iy + th};
    } else {
        const int textTop = iy + 4 + iconH + 2;
        int th = ch - (textTop - iy);
        if (th > twoLine) th = twoLine;
        if (th < 14) th = 14;
        int labLeft = 0, labW = 0;
        ItemLabelBox(idx, labLeft, labW);
        r = RECT{labLeft, textTop, labLeft + labW, textTop + th};
    }
    MapWindowPoints(hwnd_, nullptr, reinterpret_cast<LPPOINT>(&r), 2);
    out = r;
    return true;
}

// 重排本盒子图标在全局表里的相对顺序。
// 只动 owner == index_ 的那些槽位，盒子里第 k 个图标因此变成第 k 个排序结果 ——
// 布局完全由「表里的相对顺序」决定，所以排完不用通知任何人。
void FenceWindow::SortNow() {
    if (!all_ || !cfg_) return;
    const int key = cfg_->sortKey;
    if (key < 0 || key > 3) return;   // 4 = 自定义：保持用户自己拖出来的顺序

    // 只排当前分组：别的分组各自有各自的顺序，不能被这次排序带走
    const int g = activeGroup();
    std::vector<DesktopIconView*> mine;
    for (auto* e : *all_)
        if (e && e->owner == index_ && e->group == g) mine.push_back(e);
    if (mine.size() < 2) return;

    std::vector<SortInfo> info(mine.size());
    for (size_t i = 0; i < mine.size(); ++i) FillSortInfo(mine[i], info[i]);

    std::vector<size_t> idx(mine.size());
    for (size_t i = 0; i < idx.size(); ++i) idx[i] = i;

    std::stable_sort(idx.begin(), idx.end(), [&](size_t a, size_t b) {
        const SortInfo& x = info[a];
        const SortInfo& y = info[b];
        // 目录永远排前面（和资源管理器一致）
        if (x.isDir != y.isDir) return x.isDir;
        int c = 0;
        switch (key) {
        case 0:   // 名称：用逻辑排序，文件2 排在 文件10 前面
            c = StrCmpLogicalW(x.name.c_str(), y.name.c_str());
            break;
        case 1:   // 类型：按扩展名，同类再按名称
            c = StrCmpLogicalW(x.ext.c_str(), y.ext.c_str());
            if (c == 0) c = StrCmpLogicalW(x.name.c_str(), y.name.c_str());
            break;
        case 2:   // 大小：同大小按名称
            c = (x.size < y.size) ? -1 : (x.size > y.size ? 1 : 0);
            if (c == 0) c = StrCmpLogicalW(x.name.c_str(), y.name.c_str());
            break;
        default:  // 修改日期：同一时刻按名称
            c = CompareFileTime(&x.mtime, &y.mtime);
            if (c == 0) c = StrCmpLogicalW(x.name.c_str(), y.name.c_str());
            break;
        }
        return c < 0;
    });

    if (!cfg_->sortAsc) std::reverse(idx.begin(), idx.end());

    std::vector<DesktopIconView*> ordered;
    ordered.reserve(idx.size());
    for (size_t i : idx) ordered.push_back(mine[i]);

    size_t k = 0;
    for (size_t i = 0; i < all_->size() && k < ordered.size(); ++i) {
        DesktopIconView* e = (*all_)[i];
        if (e && e->owner == index_ && e->group == g) (*all_)[i] = ordered[k++];
    }
}

bool FenceWindow::ItemOrigin(int index, int& x, int& y) const {
    if (index < 0) return false;
    const int cols = CellCols();
    const int c = index % cols;
    const int r = index / cols;
    const int ch = cellH();
    if (IsListView()) {
        x = kPad;
    } else {
        x = ColCenterX(c, cols) - iconPx() / 2;   // 图标以列中心居中对齐
    }
    y = HeadH() + kPad + r * ch - scrollY_;
    // 只有「整行都落在内容区里」才算可见：滚动按整行对齐，所以不会出现半行，
    // 也就不需要裁剪 —— 上半行不会漏到头部，下半行不会漏到卡片圆角外。
    return y >= HeadH() + kPad && y + ch <= curH_ - kPad;
}

// ---- 内容纵向滚动 ----

// 最多能往下滚多少（一定是 cellH 的整数倍）
int FenceWindow::MaxScroll() const {
    if (collapsed_ || !all_) return 0;
    std::vector<DesktopIconView*> mine;
    CollectVisible(mine);
    const int cols = std::max(1, CellCols());
    const int rows = ((int)mine.size() + cols - 1) / cols;
    const int ch = cellH();
    if (ch <= 0) return 0;
    const int top = HeadH() + kPad;
    const int viewRows = std::max(0, (curH_ - kPad - top) / ch);
    if (rows <= viewRows) return 0;
    return (rows - viewRows) * ch;
}

void FenceWindow::ClampScroll() {
    const int ch = cellH();
    if (scrollY_ < 0) scrollY_ = 0;
    const int maxS = MaxScroll();
    if (scrollY_ > maxS) scrollY_ = maxS;
    if (ch > 0) scrollY_ = (scrollY_ / ch) * ch;   // 对齐到整行
}

void FenceWindow::ScrollBy(int deltaPx) {
    const int old = scrollY_;
    scrollY_ += deltaPx;
    ClampScroll();
    if (scrollY_ != old) Redraw();
}

// 把第 index 个图标滚进可见范围。
// 视口高度按「展开高度」算而不是 curH_：定位往往紧跟在一次展开动画之后，
// 那时的 curH_ 可能还是 4px，用它算出来永远是「一行都放不下」。
void FenceWindow::ScrollItemIntoView(int index) {
    if (index < 0) return;
    const int ch = cellH();
    if (ch <= 0) return;
    const int cols = std::max(1, CellCols());
    const int top = HeadH() + kPad;
    int viewRows = (height_ - kPad - top) / ch;
    if (viewRows < 1) viewRows = 1;

    const int rowTop = (index / cols) * ch;
    const int rowBottom = rowTop + ch;
    int want = scrollY_;
    if (rowTop < scrollY_) {
        want = rowTop;                                          // 在上面 → 顶到最上
    } else if (rowBottom > scrollY_ + viewRows * ch) {
        want = rowBottom - viewRows * ch;                        // 在下面 → 顶到最下
    }
    want = (want / ch) * ch;   // 整行对齐，和滚轮同一套规则
    const int old = scrollY_;
    scrollY_ = want;
    ClampScroll();
    if (scrollY_ != old) Redraw();
}

void FenceWindow::LocateItem(DesktopIconView* e) {
    if (!e) return;
    // 它在别的分组：先切过去，否则 CollectVisible 里根本找不到它
    if (e->group != activeGroup()) SetActiveGroup(e->group);
    locatePending_ = e;
    if (collapsed_ || expandMode() == 0) SetCollapsed(false);
    // 还在展开动画里：滚动量得等高度落定再算，交给 WM_TIMER
    if (curH_ + 1 < height_) return;
    DoLocateNow(e);
}

void FenceWindow::DoLocateNow(DesktopIconView* e) {
    locatePending_ = nullptr;
    if (!e || !all_ || e->owner != index_) return;
    std::vector<DesktopIconView*> mine;
    CollectVisible(mine);
    int idx = -1;
    for (size_t i = 0; i < mine.size(); ++i) {
        if (mine[i] == e) {
            idx = (int)i;
            break;
        }
    }
    if (idx < 0) return;   // 不在当前分组 / 不是这个盒子的，没什么可定位的
    ScrollItemIntoView(idx);
    StartFlash(e);
}

void FenceWindow::StartFlash(DesktopIconView* e) {
    flash_ = e;
    flashStep_ = 0;
    if (hwnd_) SetTimer(hwnd_, kFlashTimer, kFlashInterval, nullptr);
    Redraw();
}

void FenceWindow::TickFlash() {
    if (!flash_) {
        if (hwnd_) KillTimer(hwnd_, kFlashTimer);
        return;
    }
    ++flashStep_;
    if (flashStep_ >= kFlashPhases) {
        flash_ = nullptr;   // 亮相 0/2，灭相 1/3 —— 数到 4 正好两下
        if (hwnd_) KillTimer(hwnd_, kFlashTimer);
    }
    Redraw();
}

RECT FenceWindow::rect() const {
    RECT r{};
    if (hwnd_) GetWindowRect(hwnd_, &r);
    return r;
}

RECT FenceWindow::bodyRect() const {
    RECT r = rect();
    // 收起后整窗只有 4px，比 HeadH() 还矮：别让 top 越过 bottom
    r.top = std::min((int)r.bottom, (int)r.top + HeadH());
    return r;
}

bool FenceWindow::HitTest(POINT screenPt) const {
    if (!hwnd_) return false;
    RECT r = rect();
    return screenPt.x >= r.left && screenPt.x < r.right &&
           screenPt.y >= r.top && screenPt.y < r.bottom;
}

// ---------------------------------------------------------------------------
// 拖动定位：屏幕边硬夹 + 别的盒子「磁吸」
//
// 历史：最早是「先算出这一帧想去哪、再按最小穿透量推回来」（一帧跳几百像素时
// 会从另一侧穿出去）；后来改成「分轴扫掠 + 硬阻挡」（穿不过去了，但盒子成了墙，
// 想把 A 挪到 B 的左边变成不可能完成的任务）。
//
// 现在：屏幕边仍然硬夹（那是物理边界），**别的盒子只提供吸附线**。
// 靠近到 kSnapPx 以内就把边缘贴上去（左边对左边、右边贴左边、右对右……），
// 继续往那个方向拖超过阈值就自然脱离，可以任意越过其他盒子。
//
// 吸附线 / 阈值在文件开头的匿名命名空间里（改尺寸那边也要用）。
// ---------------------------------------------------------------------------
void FenceWindow::ResolveDragMove(RECT from, int wantX, int wantY,
                                  int& outX, int& outY) const {
    // from 只用来取尺寸：位置一律按「这一帧想去哪」算，不再依赖起点
    const int w = (int)(from.right - from.left);
    const int h = (int)(from.bottom - from.top);
    dbgWant_ = RECT{wantX, wantY, wantX + w, wantY + h};

    // 1) 屏幕：按「矩形跟哪块屏重叠最多」选工作区（跨屏时是几块屏的并集）。
    //    以前按左上角所在屏取，盒子往第二块屏拖时左上角还在旧屏上，
    //    于是被旧屏的右边界夹住 —— 拖到交界处就过不去了。
    RECT want{wantX, wantY, wantX + w, wantY + h};
    const RECT wa = ScreenUtil::WorkAreaForRect(want);
    want = ScreenUtil::ClampRect(want, wa);
    wantX = (int)want.left;
    wantY = (int)want.top;
    want.right = wantX + w;
    want.bottom = wantY + h;

    // 2) 别的盒子：只作为吸附参考线（不再是墙）
    std::vector<RECT> others;
    if (s_obstacleProvider) others = s_obstacleProvider(this);
    dbgObstacles_ = s_obstacleProvider ? (int)others.size() : -1;

    if (!others.empty()) {
        if (g_config.autoSnap) {
            std::vector<int> xs, ys;
            CollectSnapLines(others, wa, want, xs, ys);
            wantX = SnapAxis(wantX, w, xs);
            wantY = SnapAxis(wantY, h, ys);
        }
        // 夹回工作区这一步不能放进 autoSnap 里：它既是吸附的兜底，
        // 也是「关掉吸附后盒子仍然拖不出屏幕」的唯一保证。
        wantX = std::clamp(wantX, (int)wa.left, std::max((int)wa.left, (int)wa.right - w));
        wantY = std::clamp(wantY, (int)wa.top, std::max((int)wa.top, (int)wa.bottom - h));
    }

    outX = wantX;
    outY = wantY;
    dbgGot_ = POINT{outX, outY};
}

std::wstring FenceWindow::DragDebug() const {
    return L"obstacles=" + std::to_wstring(dbgObstacles_) + L" want=(" +
           std::to_wstring(dbgWant_.left) + L"," + std::to_wstring(dbgWant_.top) + L") got=(" +
           std::to_wstring(dbgGot_.x) + L"," + std::to_wstring(dbgGot_.y) + L")";
}

DesktopIconView* FenceWindow::ItemAtScreen(POINT screenPt) const {
    if (!hwnd_ || collapsed_) return nullptr;
    POINT c = screenPt;
    ScreenToClient(hwnd_, &c);
    const int i = HitItem(c);
    if (i < 0) return nullptr;
    // 必须用和 HitItem 同一份列表取下标，否则以后 Collect 和 CollectVisible
    // 的口径一旦分叉，这里就会取到隔壁那个图标
    std::vector<DesktopIconView*> mine;
    CollectVisible(mine);
    return (i < (int)mine.size()) ? mine[i] : nullptr;
}

int FenceWindow::HitItem(POINT clientPt) const {
    // 头部 / 卡片内边距里不该命中任何图标（滚动之后第一排的格子可能伸到上面去）
    if (clientPt.y < HeadH() + kPad || clientPt.y >= curH_ - kPad) return -1;
    std::vector<DesktopIconView*> mine;
    CollectVisible(mine);
    for (size_t i = 0; i < mine.size(); ++i) {
        int ix = 0, iy = 0;
        if (!ItemOrigin((int)i, ix, iy)) continue;
        RECT r{};
        ItemRect((int)i, r);
        if (clientPt.x >= r.left && clientPt.x < r.right && clientPt.y >= r.top &&
            clientPt.y < r.bottom)
            return (int)i;
    }
    return -1;
}

void FenceWindow::CollectVisible(std::vector<DesktopIconView*>& out) const {
    // 必须是 Collect 的结果（owner + 当前分组都过滤过）：
    // 绘制 / 命中 / 落点 / flip 槽位全都走这里，一旦自己另写一份循环，
    // 切分组时就会出现「标签换了、图标还是原来那批」。
    Collect(out);
    // 正在被拖出去的那个仍然占着原来的格子：布局不能动，否则后面的图标
    // 会在松手之前就往前挤。它由 Paint() 画成半透明的「空位」。
}

// ---------------------------------------------------------------------------
// 多选
//
// 选中标志存在 DesktopIconView::selected 上，盒子这边只负责维护它：
//   · 单击        → 只选它
//   · Ctrl + 单击 → 加/减选
//   · Shift + 单击→ 从锚点到它的整段
//   · 空白处拖框  → 框住的都选（Ctrl 拖框 = 追加）
// 这样「拖一整批」「跨分组」「排序后高亮跟着走」都不用另存下标。
// ---------------------------------------------------------------------------
void FenceWindow::ClearSelection() {
    if (!all_) return;
    // 只清这个盒子里的：桌面上的选中和盒子里互不影响
    for (auto* e : *all_)
        if (e && e->owner == index_) e->selected = false;
    selAnchor_ = -1;
}

std::vector<DesktopIconView*> FenceWindow::Selection() const {
    std::vector<DesktopIconView*> out;
    Collect(out);   // 按显示顺序
    out.erase(std::remove_if(out.begin(), out.end(),
                             [](DesktopIconView* e) { return !e || !e->selected; }),
              out.end());
    return out;
}

void FenceWindow::SelectRange(int a, int b, bool keepOthers) {
    std::vector<DesktopIconView*> mine;
    Collect(mine);
    if (mine.empty()) return;
    if (a > b) std::swap(a, b);
    a = std::max(0, a);
    b = std::min((int)mine.size() - 1, b);
    if (!keepOthers) {
        for (auto* e : mine) e->selected = false;
    }
    for (int i = a; i <= b; ++i) mine[i]->selected = true;
}

void FenceWindow::UpdateMarqueeSelection() {
    std::vector<DesktopIconView*> mine;
    CollectVisible(mine);
    RECT m{std::min(marqueeA_.x, marqueeB_.x), std::min(marqueeA_.y, marqueeB_.y),
           std::max(marqueeA_.x, marqueeB_.x), std::max(marqueeA_.y, marqueeB_.y)};
    for (size_t i = 0; i < mine.size(); ++i) {
        RECT r{};
        ItemRect((int)i, r);
        const bool hit = (r.left < m.right && m.left < r.right && r.top < m.bottom &&
                          m.top < r.bottom);
        if (hit) mine[i]->selected = true;
        else if (!marqueeAdditive_) mine[i]->selected = false;
    }
}

int FenceWindow::itemCount() const {
    std::vector<DesktopIconView*> mine;
    CollectVisible(mine);
    return (int)mine.size();
}

void FenceWindow::ClearDropHint() {
    if (drop_.item == -1 && drop_.insert == -1) return;
    drop_ = DropHint{};
    Redraw();
}

// 放弃盒内这次拖动，图标回到原来的位置（交棒给系统级拖放之前调）。
// 按下时留下的 press* 状态也要清：交棒之后 OLE 的循环还会把鼠标移动派发进来，
// pressBody_/pressItem_ 还留着的话会被当成「又开始拖了」。
void FenceWindow::AbortItemsDrag() {
    if (!itemDragging_) return;
    if (hwnd_ && GetCapture() == hwnd_) ReleaseCapture();
    itemDragging_ = false;
    dragged_ = nullptr;
    dragItems_.clear();
    pressBody_ = false;
    pressItem_ = -1;
    pressHeader_ = false;
    pressTab_ = -1;
    drop_ = DropHint{};
    Redraw();
}

void FenceWindow::UpdateDropHint(POINT screenPt) {
    if (!hwnd_) return;
    RECT wr = rect();
    // 收起后这条把手只有 4px 高，拿它当拖放入口太苛刻了：
    // 拖着图标过来时判定范围往下放宽一点，够到就把盒子拉开展示。
    if (collapsed_) wr.bottom += 28;
    const bool inside = screenPt.x >= wr.left && screenPt.x < wr.right &&
                        screenPt.y >= wr.top && screenPt.y < wr.bottom;

    if (!inside) {
        ClearDropHint();
        return;
    }

    POINT pt{screenPt.x - wr.left, screenPt.y - wr.top};

    // 拖到收起盒子的头部 → 自动展开，才能看清要插到哪
    if (collapsed_ && pt.y < HeadH()) {
        autoExpanded_ = true;
        SetCollapsed(false);
        return;
    }
    if (collapsed_) {
        ClearDropHint();
        return;
    }

    DropHint d;
    const int it = (pt.y >= HeadH()) ? HitItem(pt) : -1;
    if (it >= 0) {
        // 分区按「图标中心」算（ItemHighlightRect），不用命中用的无缝格子 ——
        // 首列的命中格子被夹到 kPad，拿它切三段会让「左/右」偏掉
        RECT ir = ItemHighlightRect(it);
        const int cw = ir.right - ir.left;
        const int third = cw / 3;
        const int rx = pt.x - ir.left;
        d.item = it;
        if (rx < third) {
            d.zone = -1;
            d.insert = it;
        } else if (rx >= cw - third) {
            d.zone = 1;
            d.insert = it + 1;
        } else {
            d.zone = 0;   // 中间：以拖拽文件打开目标图标的程序
            d.insert = it;
        }
    }
    if (d.item != drop_.item || d.zone != drop_.zone || d.insert != drop_.insert) {
        drop_ = d;
        Redraw();
    }
}

LRESULT FenceWindow::WndProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    // 系统原生右键菜单弹出期间，带图标的项（打开方式 / 发送到 …）是 shell 自绘的，
    // 必须把这三条消息转给它，否则那些项会塌成 0 高度看不见。
    if (msg == WM_INITMENUPOPUP || msg == WM_MEASUREITEM || msg == WM_DRAWITEM) {
        if (ForwardShellMenuMessage(h, msg, wp, lp)) return TRUE;
    }

    switch (msg) {
    case WM_MOUSEACTIVATE:
        return MA_NOACTIVATE;

    case WM_LBUTTONDOWN: {
        POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        BeginPress(h, pt);
        Redraw();
        return 0;
    }

    // 双击图标才打开（单击只选中）。窗口类必须带 CS_DBLCLKS 才会收到这个消息。
    case WM_LBUTTONDBLCLK: {
        POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        BeginPress(h, pt);

        // 双击分组标签 → 改名（0 = 盒子标题）。这里必须把 pressTab_ 清掉：
        // 紧接着那次 LBUTTONUP 会把它当成「单击切分组」，刚点开弹窗分组就跳走了。
        if (pressTab_ >= 0) {
            const int t = pressTab_;
            pressTab_ = -1;
            pressHeader_ = false;   // 别让这次双击顺带把盒子拖起来
            pressBody_ = false;
            itemDragging_ = false;
            dragging_ = false;
            Redraw();
            if (onRenameGroup) onRenameGroup(t);
            return 0;
        }

        if (!resizing_ && pressItem_ >= 0) {
            const int item = pressItem_;
            std::vector<DesktopIconView*> mine;
            Collect(mine);
            // 双击 = 「只留这一个」，不然多选状态下双击打开还会残留一堆高亮
            ClearSelection();
            if (item < (int)mine.size()) {
                mine[item]->selected = true;
                selAnchor_ = item;
            }
            pressItem_ = -1;    // 随后的 LBUTTONUP 别再打开一次
            pressBody_ = false;
            Redraw();
            if (item < (int)mine.size() && onOpen) onOpen(mine[item]);
        }
        return 0;
    }

    case WM_SETCURSOR: {
        if (LOWORD(lp) == HTCLIENT && GetCapture() != h) {
            POINT pt{};
            GetCursorPos(&pt);
            ScreenToClient(h, &pt);
            // 锁按钮上给手型（它和头部拖动长得很近，得有个区分）。
            // 收起态的 4px 把手上什么都没有，直接跳过这一套。
            if (showTitle_ && !collapsed_ && pt.y < HeadH()) {
                const RECT lr = LockRect();
                if (pt.x >= lr.left && pt.x < lr.right && pt.y >= lr.top && pt.y < lr.bottom) {
                    SetCursor(LoadCursorW(nullptr, IDC_HAND));
                    return TRUE;
                }
                if (HitTab(pt) >= 0) {
                    SetCursor(LoadCursorW(nullptr, IDC_HAND));
                    return TRUE;
                }
            }
            LPCWSTR id = nullptr;
            switch (HitEdge(pt)) {
            case Edge::L: case Edge::R: id = IDC_SIZEWE; break;
            case Edge::T: case Edge::B: id = IDC_SIZENS; break;
            case Edge::TL: case Edge::BR: id = IDC_SIZENWSE; break;
            case Edge::TR: case Edge::BL: id = IDC_SIZENESW; break;
            default: break;
            }
            if (id) {
                SetCursor(LoadCursorW(nullptr, id));
                return TRUE;
            }
        }
        break;
    }

    case WM_MOUSEMOVE: {
        POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        POINT cur{};
        GetCursorPos(&cur);

        if (GetCapture() == h) {
            const int dx = cur.x - pressScreenPt_.x;
            const int dy = cur.y - pressScreenPt_.y;

            if (resizing_) {
                ApplyResize(cur);
                return 0;
            }

            if ((pressHeader_ || pressArrow_) && !itemDragging_) {
                if (!dragging_ && (std::abs(dx) > 2 || std::abs(dy) > 2)) {
                    dragging_ = true;
                    // 从这一刻起按「每帧鼠标位移」跟手，而不是「按下点 + 总位移」。
                    // 用总位移的话，被夹住的那段距离会一直欠着 —— 鼠标得原路
                    // 退回被夹住的距离盒子才重新动，拖到边上就像卡死了。
                    dragLastCursor_ = cur;
                    // 未吸附的「原始位置」从当前实际位置起算
                    dragRawX_ = dragLastX_;
                    dragRawY_ = dragLastY_;
                    // 底图在「按下」那一刻就抓好了（见 BeginPress），这里通常什么都不做。
                    // 兜底：按下之后才打开磨砂 / 那次抓失败了，这里补一次；
                    // 同步抓完再动，异步会和前几帧抢 win32k 的 GDI 锁反而更卡。
                    if (!pressCaptured_ && cfg_ && cfg_->acrylic) {
                        pressCaptured_ = true;
                        RefreshBackdrop(128);
                    }
                }
                if (dragging_) {
                    const int mdx = cur.x - dragLastCursor_.x;
                    const int mdy = cur.y - dragLastCursor_.y;
                    if (mdx || mdy) {
                        dragLastCursor_ = cur;
                        const int fw = (int)(dragStartRect_.right - dragStartRect_.left);
                        // 收起时窗口只有 4px 高，位置夹取和吸附要按「展开后的大小」算，
                        // 否则盒子能被拖到贴着屏幕底边，一展开就出屏了
                        const int fh2 =
                            collapsed_ ? height_
                                       : (int)(dragStartRect_.bottom - dragStartRect_.top);

                        // 累积到**未吸附**的原始位置上，再让吸附只影响「摆到哪儿」。
                        //
                        // 这一步是修「吸附之后盒子就动不了」：以前每帧都拿
                        // 「吸附后的位置 + 这一帧位移」去算，一旦吸住，下一帧只移动
                        // 1~2px 又落回吸附范围里，又被吸回去 —— 慢慢拖的时候盒子
                        // 永远离不开那条线。现在原始位置该走多少走多少，鼠标累计
                        // 离开阈值，吸附自然就断了。
                        dragRawX_ += mdx;
                        dragRawY_ += mdy;
                        // 原始位置也要夹在屏幕里：否则鼠标推到屏幕外几百像素后，
                        // 再往回拖要先把这段「欠账」还完，手感像卡住。
                        {
                            const RECT raw{dragRawX_, dragRawY_, dragRawX_ + fw,
                                           dragRawY_ + fh2};
                            const RECT c = ScreenUtil::ClampRect(
                                raw, ScreenUtil::WorkAreaForRect(raw));
                            dragRawX_ = (int)c.left;
                            dragRawY_ = (int)c.top;
                        }

                        int nx = dragLastX_, ny = dragLastY_;
                        ResolveDragMove(RECT{dragRawX_, dragRawY_, dragRawX_ + fw,
                                             dragRawY_ + fh2},
                                        dragRawX_, dragRawY_, nx, ny);
                        if (nx != dragLastX_ || ny != dragLastY_) {
                            dragLastX_ = nx;
                            dragLastY_ = ny;
                            SetWindowPos(h, nullptr, nx, ny, 0, 0,
                                         SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
                            // 窗口一动，位图里的底图也得按新偏移重画一遍，
                            // 否则磨砂会「整块跟着窗口平移」（等于背景冻住了）。
                            if (cfg_ && cfg_->acrylic) {
                                EnsureBackdropCover();
                                Redraw();
                            }
                        }
                    }
                    return 0;
                }
            }

            // 拖图标优先：一旦拖起来就一直走这里，哪怕中途因为划过标签切了分组、
            // pressItem_ 已经被清成 -1，也不能掉到下面的框选分支去。
            if (itemDragging_) {
                // 拖着图标划到某个分组标签上 → 立刻切过去，松手就落进那个分组。
                // 这里不能只靠下面的悬停逻辑：鼠标已被 SetCapture 抓住，
                // 上面早就 return 了，根本走不到那段。
                if (showTitle_ && !collapsed_ && pt.y >= 0 && pt.y < HeadH()) {
                    const int t = HitTab(pt);
                    if (t >= 0 && t != activeGroup()) SetActiveGroup(t);
                }
                if (onItemsDragMove) onItemsDragMove(cur);
                return 0;
            }

            if (pressBody_ && pressItem_ >= 0) {
                if (std::abs(dx) > 4 || std::abs(dy) > 4) {
                    std::vector<DesktopIconView*> mine;
                    Collect(mine);
                    if (pressItem_ < (int)mine.size()) {
                        DesktopIconView* e = mine[pressItem_];
                        // 按下的那个不在选中集里 → 先切成「只选它」，再拖它一个。
                        // 在选中集里 → 拖着整批走（多选拖出）。
                        if (!e->selected) {
                            ClearSelection();
                            e->selected = true;
                            selAnchor_ = pressItem_;
                        }
                        itemDragging_ = true;
                        dragged_ = e;
                        dragItems_ = Selection();
                        // 主图标（光标下面那个）放最前面：浮层和落点都以它为准
                        auto it = std::find(dragItems_.begin(), dragItems_.end(), e);
                        if (it != dragItems_.end()) std::rotate(dragItems_.begin(), it, it + 1);
                        if (onItemsDragStart) onItemsDragStart(dragItems_, cur);
                    }
                    Redraw();
                }
            }

            // 空白处拖动 = 框选（和桌面同一个操作习惯）
            if (pressBody_ && pressItem_ < 0 && !itemDragging_) {
                if (!marquee_ && (std::abs(dx) > 3 || std::abs(dy) > 3)) {
                    marquee_ = true;
                    marqueeA_ = pressPt_;
                    if (!marqueeAdditive_) ClearSelection();
                }
                if (marquee_) {
                    marqueeB_ = pt;
                    UpdateMarqueeSelection();
                    Redraw();
                    return 0;
                }
            }
        }

        // 伸缩模式：收起状态下鼠标移到头部（那条 4px 把手）就展开，
        // 移开整个盒子在 MOUSELEAVE 里收起。展开模式的盒子根本不收起，不必进来。
        // suppressAutoExpand_：刚手动收起时光标还压在把手上，抑制一次，
        // 否则「点了收起又立刻弹开」。
        if (expandMode() == 0 && collapsed_ && !suppressAutoExpand_ && pt.y < HeadH()) {
            autoExpanded_ = true;
            SetCollapsed(false);
        }

        int hot = -1;
        if (!collapsed_ && pt.y >= HeadH()) hot = HitItem(pt);
        if (hot != hot_) {
            hot_ = hot;
            Redraw();
        }

        // 分组标签：设置成「鼠标移入就切」时，光标一落到别的标签上就立刻切
        // （以前有 200ms 延迟定时器，用户实测觉得"卡了一下"，去掉了）。
        // 设置成「点击才切」时这里什么都不做，交给 WM_LBUTTONUP 里的 tabPress 分支。
        if (g_config.switchTabOnHover && showTitle_ && !collapsed_ && pt.y < HeadH()) {
            const int t = HitTab(pt);
            if (t >= 0 && t != activeGroup()) SetActiveGroup(t);
        }

        if (!trackingLeave_) {
            TRACKMOUSEEVENT tme{sizeof(tme), TME_LEAVE, h, 0};
            if (TrackMouseEvent(&tme)) trackingLeave_ = true;
        }
        return 0;
    }

    case WM_MOUSELEAVE:
        trackingLeave_ = false;
        suppressAutoExpand_ = false;   // 离开过一次，下次悬停又能展开了
        if (hot_ != -1) { hot_ = -1; Redraw(); }
        // 原生菜单 / 改名输入框是模态的（或至少盖在盒子上），它们一出现鼠标就「离开」了
        // 盒子；这时候不能把盒子收起来，否则菜单或正在改的名字底下直接缩没了。
        //
        // 只有伸缩模式才自动收（展开模式 = 始终展开）。也不再要求这次展开必须由
        // 悬停触发：伸缩模式的定义就是「鼠标不在就收起来」，盒子刚建出来是开着的，
        // 用户第一次移开也该收。
        if (expandMode() == 0 && !menuOpen_ && !holdOpen_) {
            if (itemDragging_ || dragging_) {
                autoExpanded_ = false;   // 正在操作，别收
            } else {
                autoExpanded_ = false;
                SetCollapsed(true);
            }
        }
        return 0;

    case WM_MOUSEWHEEL: {
        // 图标多到超出盒子高度时，滚轮往里滚。一次 3 行，按整行对齐。
        const int delta = GET_WHEEL_DELTA_WPARAM(wp);
        if (delta != 0) {
            const int step = std::max(1, 3) * cellH();
            ScrollBy(delta > 0 ? -step : step);
        }
        return 0;
    }

    case WM_RBUTTONUP: {
        // 右键图标 → 系统原生菜单（资源管理器那套）；右键空白/头部 → 我们自己的盒子菜单
        POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        POINT sp = pt;
        ClientToScreen(h, &sp);
        const int it = (pt.y >= HeadH() && !collapsed_) ? HitItem(pt) : -1;
        // 右键那个图标没被选中 → 先切成「只选它」，和桌面层右键的手感一致
        if (it >= 0) {
            std::vector<DesktopIconView*> mine;
            Collect(mine);
            if (it < (int)mine.size() && !mine[it]->selected) {
                ClearSelection();
                mine[it]->selected = true;
                selAnchor_ = it;
                Redraw();
            }
        }
        const bool nativeMenu = (it >= 0);
        menuOpen_ = nativeMenu;
        if (onContextMenu) onContextMenu(it, sp);
        menuOpen_ = false;
        // 菜单关掉之后鼠标多半还停在外面：该收就收，别留一个开着却没人操作的盒子
        if (nativeMenu && expandMode() == 0) {
            POINT cur{};
            GetCursorPos(&cur);
            if (!HitTest(cur)) {
                autoExpanded_ = false;
                SetCollapsed(true);
            }
        }
        return 0;
    }

    case WM_LBUTTONUP: {
        POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        POINT cur{};
        GetCursorPos(&cur);
        if (GetCapture() == h) ReleaseCapture();

        const int item = pressItem_;
        const bool arrow = pressArrow_;
        const bool lockPress = pressLock_;
        const int tabPress = pressTab_;
        const bool wasDragging = dragging_;
        const bool wasItemDrag = itemDragging_;
        const bool wasResizing = resizing_;
        const bool wasMarquee = marquee_;

        dragging_ = false;
        itemDragging_ = false;
        resizing_ = false;
        resizeEdge_ = Edge::None;
        pressItem_ = -1;
        pressArrow_ = false;
        pressLock_ = false;
        pressTab_ = -1;
        pressHeader_ = false;
        pressBody_ = false;
        marquee_ = false;

        if (lockPress) {
            ToggleLock();   // 锁 / 解锁（SetLocked 里会存盘并重画）
            return 0;
        }

        if (wasResizing) {
            // 拉完了：现在盒子不动了，可以补抓一张真正对得上的底图
            if (cfg_ && cfg_->acrylic) RequestBackdropAsync(0);
            if (onGeometryChanged) onGeometryChanged();
            Redraw();
            return 0;
        }
        if (wasDragging) {
            // 松手不再「把重叠的盒子推开」：以前那一步会让盒子在松开的一瞬间
            // 跳一下（用户以为卡了）。现在拖动过程只吸附不阻挡，想叠就叠着，
            // 位置完全由用户决定。
            //
            // 拖完补一张贴合的底图：拖动全程用的是开局那一圈（为了不卡），
            // 挪得远的话画面和真实壁纸已经对不上了，这里一次性校正。
            if (cfg_ && cfg_->acrylic) RequestBackdropAsync(0);
            if (onGeometryChanged) onGeometryChanged();
            Redraw();
            return 0;
        }
        if (wasMarquee) {
            // 框选过程中已经实时改过选中态了，这里只要重画一帧把框去掉
            Redraw();
            return 0;
        }
        if (wasItemDrag) {
            // 用拖动开始时缓存的指针：松手后 owner 可能已经变了
            // 注意：这里临时把 itemDragging_ 恢复成 true，让 CollectVisible 仍然排除被拖的图标，
            // 这样 main 拿到的 dropHint 下标和拖动过程中显示的一致。
            itemDragging_ = true;
            if (onItemsDrop && !dragItems_.empty()) onItemsDrop(dragItems_, cur);
            itemDragging_ = false;
            dragged_ = nullptr;
            dragItems_.clear();
            Redraw();
            return 0;
        }
        if (arrow) {
            // 箭头 = 展开方式开关，两态互切：
            //   展开模式（始终展开）→ 点一下变伸缩模式，当场收起来给个反馈
            //   伸缩模式（悬停展开）→ 点一下变展开模式，摊开并定住
            autoExpanded_ = false;   // 手动操作以用户意图为准
            if (expandMode() == 1) {
                SetExpandMode(0);
                // 光标还压在头部上，先抑制一次自动展开，否则收起来的那一帧
                // 就会被「悬停展开」原路弹回去
                suppressAutoExpand_ = true;
                SetCollapsed(true);
            } else {
                SetExpandMode(1);
                suppressAutoExpand_ = false;
                SetCollapsed(false);
            }
            return 0;
        }
        // 点标签：没拖动过才切分组（拖着走 = 用户只是想挪盒子）
        if (tabPress >= 0 && !wasDragging && tabPress != activeGroup()) {
            SetActiveGroup(tabPress);
            return 0;
        }
        if (item >= 0) {
            // 单击只选中，双击（WM_LBUTTONDBLCLK）才打开。
            // Ctrl / Shift 按下的那一刻状态在 BeginPress 里存好了。
            std::vector<DesktopIconView*> mine;
            Collect(mine);
            if (item < (int)mine.size()) {
                if (pressShift_ && selAnchor_ >= 0) {
                    SelectRange(selAnchor_, item, pressCtrl_);
                } else if (pressCtrl_) {
                    mine[item]->selected = !mine[item]->selected;
                    selAnchor_ = item;
                } else {
                    ClearSelection();
                    mine[item]->selected = true;
                    selAnchor_ = item;
                }
            }
            Redraw();
        } else if (pt.y >= HeadH() && !Selection().empty()) {
            ClearSelection();   // 点空白处取消选中
            Redraw();
        }
        (void)pt;
        return 0;
    }

    case WM_TIMER: {
        // 搜索「定位」的闪烁（和桌面层同一套节奏）
        if (wp == kFlashTimer) {
            TickFlash();
            return 0;
        }
        if (wp != kAnimTimer) break;
        bool busy = false;
        bool waiting = false;   // 后台补抓还没回来：保住定时器，但这一帧不重画

        // 收起/展开的高度补间
        const int diff = targetH_ - curH_;
        if (diff != 0) {
            int step = (int)std::lround(diff * 0.35);
            if (step == 0) step = diff > 0 ? 4 : -4;
            curH_ += step;
            if ((diff > 0 && curH_ > targetH_) || (diff < 0 && curH_ < targetH_))
                curH_ = targetH_;
            busy = true;
        }

        // 搜索「定位」：收起的盒子要先摊开才能算滚动量，所以等高度落定再做
        if (locatePending_ && curH_ == targetH_) {
            DoLocateNow(locatePending_);
            busy = true;
        }

        // 排序 flip 动画
        if (flipActive_) {
            if (GetTickCount() - flipT0_ >= (DWORD)flipDur_) {
                StopFlip();     // 收尾时还要再画一帧，让图标回到最终位置
            }
            busy = true;
        }

        // 分组标签的滑块补间
        if (tabPillAnim_) {
            if (GetTickCount() - tabPillT0_ >= (DWORD)tabPillDur_) tabPillAnim_ = false;
            busy = true;
        }

        // 亚克力底图补抓（在工作线程里跑）：回来了就在这里换上并重画一帧。
        // 还在等的时候**不重画** —— 拖动时每 16ms 多画一帧就是白白和鼠标抢时间。
        if (bdJob_) {
            if (bdJob_->finished.load(std::memory_order_acquire)) {
                EnsureBackdropCover();
                busy = true;
            } else {
                waiting = true;
            }
        }

        if (!busy) {
            if (!waiting) KillTimer(h, kAnimTimer);
            return 0;
        }
        Redraw();
        return 0;
    }

    case WM_CLOSE:
        // 盒子不负责退出程序（可能是被外部关掉的），交给 main 处理
        return 0;

    case WM_DESTROY:
        return 0;

    case WM_APP + 7:   // 仅 MEOW_DEBUG_HOOKS=1：自动化脚本用，wParam = key | asc<<8
        if (debugHooks_) {
            SetSort((int)(wp & 0xFF), ((wp >> 8) & 0xFF) != 0);
            return 0;
        }
        break;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

void FenceWindow::EnsureBackbuffer(int w, int h) {
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

// pad：往四周多抓一圈。
//
// 为什么要多抓一圈：以前只在「创建 / 改设置」时抓一次，盒子一拖动，磨砂里
// 的画面就冻在那儿不动（松开才跳一下）；改尺寸更糟 —— 缓冲变大了，
// 底图尺寸对不上，整块直接退回「只有半透明底色」，看起来就是「变透明了」。
//
// 截下的这一圈存成 backdrop_，同时还记着抓的时候的屏幕坐标（backdropX_/Y_）。
// 之后盒子在这个圈里怎么挪，绘制时按偏移「取子窗口」就行 —— 不用每帧重新
// 抓屏 + 模糊（那一步是几十毫秒的量级，拖动时每帧做必然卡）。
void FenceWindow::RefreshBackdrop(int pad) {
    // 同步抓的这一张比在途的后台那一单更新，作废掉，免得过两帧被旧的顶回去
    bdJob_.reset();
    backdrop_.clear();
    backdropW_ = backdropH_ = 0;
    if (!cfg_ || !cfg_->acrylic || !hwnd_) return;
    RECT r = rect();
    if (r.right <= r.left || r.bottom <= r.top) return;

    // 一律按「展开尺寸」抓：收起时只用到上面一段（行距仍是 width_，可直接裁）
    int w = width_ + pad * 2;
    int h = (height_ > 0 ? height_ : (r.bottom - r.top)) + pad * 2;
    int x = (int)r.left - pad;
    int y = (int)r.top - pad;

    // 别抓到屏幕外：BitBlt 越界的那一圈会是黑的，模糊之后整块底图偏暗。
    // 夹到虚拟桌面范围内（不是工作区 —— 盒子可能压到任务栏那一块）。
    {
        const RECT vs = ScreenUtil::VirtualScreen();
        const int x0 = std::max(x, (int)vs.left);
        const int y0 = std::max(y, (int)vs.top);
        const int x1 = std::min(x + w, (int)vs.right);
        const int y1 = std::min(y + h, (int)vs.bottom);
        if (x1 - x0 < 16 || y1 - y0 < 16) {
            backdrop_.clear();
            return;
        }
        x = x0;
        y = y0;
        w = x1 - x0;
        h = y1 - y0;
    }

    wchar_t mode[32]{};
    GetEnvironmentVariableW(L"MEOW_BACKDROP", mode, 32);
    const bool preferDesktop = _wcsicmp(mode, L"desktop") == 0;

    // 快路径：不带 CAPTUREBLT 的抓屏。
    //
    // 分层窗口（我们自己的盒子 / 桌面图标层）在不带 CAPTUREBLT 时天然不进画面，
    // 所以完全不用去动 SetWindowDisplayAffinity —— 那一步每次都会让 DWM 把整个
    // 桌面重新合成一遍，还要 EnumWindows 扫一遍所有窗口，抓完再改回来又是两遍。
    // 拖动时每 100ms 来一轮，看到的就是「磨砂一卡一卡」。
    bool ok = false;
    bool uni = false;
    if (!preferDesktop) ok = CaptureBlurredBackdropFast(x, y, w, h, backdrop_, &uni);

    // uni：快路径抓到的是一片纯色 —— 可能是壁纸真的纯色，也可能是壁纸画在分层
    // 窗口上（快路径看不见它）。这条同步路径只在「首次 / 换壁纸 / 后台失败兜底」时走，
    // 宁可慢一点也要拿对，所以一律让慢路径再确认一次。
    if (!ok || uni) {
        // 慢路径：壁纸是分层窗口画的（某些动态壁纸），快路径抓到一片纯色/全黑。
        // 这时必须带 CAPTUREBLT 把分层窗口也抓进来，同时把所有自家窗口排除掉
        // （否则会抓到上一帧的自己，形成回授）。
        const bool hook = s_captureExcluder != nullptr;
        if (hook) s_captureExcluder(RECT{x, y, x + w, y + h}, true);
        SetWindowDisplayAffinity(hwnd_, WDA_EXCLUDEFROMCAPTURE);
        ok = CaptureBlurredBackdrop(x, y, w, h, 20, backdrop_, preferDesktop);
        SetWindowDisplayAffinity(hwnd_, WDA_NONE);
        if (hook) s_captureExcluder(RECT{x, y, x + w, y + h}, false);
    }
    if (!ok) {
        backdrop_.clear();
        return;
    }
    backdropW_ = w;
    backdropH_ = h;
    backdropX_ = x;
    backdropY_ = y;
    backdropCoverT_ = GetTickCount();
}

// 提交一次后台抓取：矩形 = 当前盒子 + 四周 pad，夹在虚拟桌面内。
void FenceWindow::RequestBackdropAsync(int pad) {
    if (!cfg_ || !cfg_->acrylic || !hwnd_ || bdJob_) return;
    RECT r = rect();
    if (r.right <= r.left || r.bottom <= r.top) return;
    const int needW = width_ > 0 ? width_ : (int)(r.right - r.left);
    const int needH = height_ > 0 ? height_ : (int)(r.bottom - r.top);
    // 比屏幕还大 / 坐标跑到虚拟桌面外的老配置：永远补不全，别一直提交
    const RECT vs = ScreenUtil::VirtualScreen();
    if (needW + 4 > (int)(vs.right - vs.left) || needH + 4 > (int)(vs.bottom - vs.top))
        return;
    RECT want{(int)r.left - pad, (int)r.top - pad, (int)r.left + needW + pad,
              (int)r.top + needH + pad};
    want.left = std::max((int)want.left, (int)vs.left);
    want.top = std::max((int)want.top, (int)vs.top);
    want.right = std::min((int)want.right, (int)vs.right);
    want.bottom = std::min((int)want.bottom, (int)vs.bottom);
    bdJob_ = BackdropCaptureAsync(want);
    // 结果要靠定时器收（鼠标停住时没有鼠标消息）
    if (bdJob_ && hwnd_) SetTimer(hwnd_, kAnimTimer, 16, nullptr);
}

// 补磨砂底图。
//
// 【这一版最重要的约束】拖动 / 改尺寸期间**一次都不许抓屏**。
//
// 为什么：BitBlt 抓屏要拿 win32k 的全局 GDI 锁，还会把 DWM 合成好的桌面读回来。
// 换到工作线程也没用 —— UI 线程下一帧的 UpdateLayeredWindow / SetWindowPos 一样要排
// 那把锁；而且「拖到一半突然换一张底图」本身就让背景一跳一跳的，比卡还难看。
// 上一版就是边拖边在后台抓，用户反馈「背景变的更卡了」。
//
// 所以只在真正安全的时候抓：
//   · 拖动 / 改尺寸**开始**那一刻（盒子还没动，同步抓一圈带余量的，用户看不见）
//   · 松手之后（WM_LBUTTONUP）、改设置、换壁纸（都是静止状态）
// 移动过程中底图不够用时，Paint 会把取样原点夹回底图内（整块磨砂跟着盒子平移几像素，
// 模糊过的画面看不出来），绝不为了「跟准」去抓屏。
void FenceWindow::EnsureBackdropCover() {
    if (!cfg_ || !cfg_->acrylic || !hwnd_) return;

    // 先看后台那一单有没有回来
    if (bdJob_) {
        if (!bdJob_->finished.load(std::memory_order_acquire)) return;   // 还在抓
        if (bdJob_->ok && (!bdJob_->uniform || bdPlainArea_ || backdrop_.empty())) {
            // 用上了。uniform 的两种含义见 CaptureBlurredBackdropFast 的注释：
            // 已知这块地方就是纯色（bdPlainArea_）、或者现在压根没有底图时，照用。
            backdrop_ = std::move(bdJob_->px);
            backdropW_ = bdJob_->w;
            backdropH_ = bdJob_->h;
            backdropX_ = bdJob_->x;
            backdropY_ = bdJob_->y;
            backdropCoverT_ = GetTickCount();
            if (!bdJob_->uniform) bdPlainArea_ = false;
            bdJob_.reset();
            return;
        }
        const bool wasUniform = bdJob_->uniform;
        bdJob_.reset();
        // 第一次抓到「一片纯色」：分不清是壁纸本来就纯色，还是壁纸是分层窗口画的
        // （快路径看不见它）。用慢路径确认一次；确认下来也是纯色就记住，以后不折腾。
        const DWORD nowT = GetTickCount();
        if (wasUniform && nowT - backdropCoverT_ >= 500) {
            backdropCoverT_ = nowT;
            RefreshBackdrop(0);
            bdPlainArea_ = !backdrop_.empty() && NearlyUniform(backdrop_, backdropW_,
                                                              backdropH_);
            return;
        }
        // 其它情况（抓屏直接失败）：慢路径兜底，限频 300ms
        if (nowT - backdropCoverT_ >= 300) {
            backdropCoverT_ = nowT;
            RefreshBackdrop(0);
        }
        return;
    }

    if (backdrop_.empty()) {
        // 还没有底图（首次 / 后台抓不到）：只能同步抓一次，否则整块没有磨砂
        const DWORD now0 = GetTickCount();
        if (now0 - backdropCoverT_ < 200) return;
        backdropCoverT_ = now0;
        RefreshBackdrop(dragging_ || resizing_ ? 128 : 0);
        return;
    }

    RECT r = rect();
    const int needW = width_ > 0 ? width_ : (int)(r.right - r.left);
    const int needH = height_ > 0 ? height_ : (int)(r.bottom - r.top);
    const int sx = (int)r.left - backdropX_;
    const int sy = (int)r.top - backdropY_;
    const bool covered = sx >= 0 && sy >= 0 && sx + needW <= backdropW_ &&
                         sy + needH <= backdropH_;
    if (covered) return;

    if (dragging_ || resizing_) {
        // 移动中：什么都不做。底图不够用就让它将就着（Paint 会把取样原点夹回来），
        // 等松手那一刻再补一张贴合的（见 WM_LBUTTONUP）。
        return;
    }
    // 静止状态（刚拖完 / 改完设置 / 换壁纸）：交给后台线程，别阻塞界面。
    // pad = 0：静止时不用留余量，下次开始拖动那一瞬间会重抓一圈大的。
    RequestBackdropAsync(0);
}

void FenceWindow::Paint() {
    if (!bits_ || !memDC_ || !cfg_) return;
    std::memset(bits_, 0, (size_t)bufW_ * (size_t)bufH_ * 4);

    // 关掉「显示盒子标题」时没有箭头可点，也就收不起来；万一配置切换把它卡在
    // 收起态，就整个盒子只留一条 2px 感应条（见 HeadH）。
    // 填 alpha=1 —— 肉眼看不见，但 per-pixel alpha 分层窗口里 alpha>0 的像素
    // 才能收到鼠标消息，悬停自动展开靠的就是它。
    if (!showTitle_ && collapsed_) {
        auto* p = reinterpret_cast<uint32_t*>(bits_);
        const size_t n = (size_t)bufW_ * (size_t)std::max(1, bufH_);
        for (size_t i = 0; i < n; ++i) p[i] = 0x01000000u;
        return;
    }

    const COLORREF base = (COLORREF)cfg_->color;
    const int alpha = std::clamp(cfg_->alpha, 0, 255);
    // 圆角最多 20：和设置页滑块的上限保持一致（这里再夹一次，防止旧配置绕过）
    const double rad = (double)std::clamp(cfg_->radius, 0, 20);
    const bool light = IsLight(base);

    const BYTE r = GetRValue(base), g = GetGValue(base), b = GetBValue(base);

    // 底图不必和缓冲等大：拖动 / 改尺寸时抓的是一圈更大的区域，这里按
    // 「当前窗口相对抓取原点」的偏移取子矩形。尺寸对不上不再等于「没有底图」——
    // 以前正是这条要求让改尺寸的整个过程都退回纯半透明（看着像变透明了）。
    RECT cur{};
    if (hwnd_) GetWindowRect(hwnd_, &cur);
    const int bsx = (int)cur.left - backdropX_;   // 底图里对应的左上角
    const int bsy = (int)cur.top - backdropY_;
    const bool hasBackdrop = cfg_->acrylic && !backdrop_.empty() && backdropW_ > 0 &&
                             backdropH_ > 0 &&
                             backdrop_.size() >= (size_t)backdropW_ * (size_t)backdropH_ * 4;

    {
        Bitmap target(bufW_, bufH_, bufW_ * 4, PixelFormat32bppPARGB,
                      reinterpret_cast<BYTE*>(bits_));
        Graphics gfx(&target);
        gfx.SetSmoothingMode(SmoothingModeAntiAlias);
        gfx.SetPixelOffsetMode(PixelOffsetModeHighQuality);
        gfx.SetTextRenderingHint(TextRenderingHintAntiAlias);

        const RectF full(0.0f, 0.0f, (float)bufW_, (float)bufH_);
        GraphicsPath shape;
        AddRoundRect(shape, full, (float)rad);

        // ---- 卡片底 ----
        if (hasBackdrop) {
            // 亚克力：模糊壁纸打底 + 半透明着色，整体不透明。
            //
            // 拖动 / 改尺寸期间不抓屏（见 EnsureBackdropCover 的说明），所以底图
            // 常常「盖不住」当前窗口。这时候把**取样原点夹回底图内部**就够了：
            // 整块磨砂相对盒子平移几个像素，模糊过的画面完全看不出来，
            // 而且仍然是一次 1:1 的整块拷贝，不额外花时间。
            //
            // 别在这儿加「把越界的边用拉伸补上」那种处理：那要 4 次带缩放的
            // DrawImage，拖动时每帧多好几毫秒，正是之前「更卡了」的原因之一。
            Bitmap blur(backdropW_, backdropH_, backdropW_ * 4, PixelFormat32bppPARGB,
                        backdrop_.data());
            if (backdropW_ >= bufW_ && backdropH_ >= bufH_) {
                const int cx = std::clamp(bsx, 0, backdropW_ - bufW_);
                const int cy = std::clamp(bsy, 0, backdropH_ - bufH_);
                gfx.SetClip(&shape);
                gfx.DrawImage(&blur, Rect(0, 0, bufW_, bufH_), cx, cy, bufW_, bufH_,
                              UnitPixel);
                gfx.ResetClip();
            } else {
                // 底图比缓冲还小：主要是「改尺寸」把盒子拉得超出了开始那一刻抓的余量，
                // 也可能是贴着屏幕边缘抓不全。这里按交集画，再用**最近的一列/一行撑开**
                // 缺的那一条 —— 模糊过的画面没有细节，拉伸看不出来；不然那一侧会变成
                // 只有半透明着色，看起来就是「拉大之后盒子变透明了」（以前的老毛病）。
                //
                // 注意：这条分支只在底图比缓冲**小**时才会走到，正常的拖动（底图比
                // 缓冲大）走上面那条 1:1 夹取，一次 DrawImage 就完事，别把拉伸挪上去。
                const int dx = std::max(0, -bsx);
                const int dy = std::max(0, -bsy);
                const int cx = std::max(0, bsx);
                const int cy = std::max(0, bsy);
                const int cw = std::min(bufW_ - dx, backdropW_ - cx);
                const int chh = std::min(bufH_ - dy, backdropH_ - cy);
                if (cw > 0 && chh > 0) {
                    gfx.SetClip(&shape);
                    gfx.DrawImage(&blur, Rect(dx, dy, cw, chh), cx, cy, cw, chh, UnitPixel);
                    if (dx > 0)
                        gfx.DrawImage(&blur, Rect(0, dy, dx, chh), cx, cy, 1, chh, UnitPixel);
                    if (dx + cw < bufW_)
                        gfx.DrawImage(&blur, Rect(dx + cw, dy, bufW_ - dx - cw, chh),
                                      cx + cw - 1, cy, 1, chh, UnitPixel);
                    if (dy > 0)
                        gfx.DrawImage(&blur, Rect(0, 0, bufW_, dy), cx, cy, cw, 1, UnitPixel);
                    if (dy + chh < bufH_)
                        gfx.DrawImage(&blur, Rect(0, dy + chh, bufW_, bufH_ - dy - chh), cx,
                                      cy + chh - 1, cw, 1, UnitPixel);
                    gfx.ResetClip();
                }
            }
            SolidBrush tint{Color((BYTE)std::min(255, (int)(alpha * 0.58)), r, g, b)};
            gfx.FillPath(&tint, &shape);
        } else {
            SolidBrush body{Color((BYTE)alpha, r, g, b)};
            gfx.FillPath(&body, &shape);
        }

        // ---- 头部：略亮、略实，做出层叠感 ----
        // 关掉「显示盒子标题」时整个头部都不画（标题、箭头、分隔线一起消失），
        // 卡片就是一整块圆角面板，顶部只保留 HeadH()=2px 的悬停感应区。
        //
        // 收起时头部整块塌到 4px：头部内容跟着当前高度一路淡出（headFade），
        // 卡片底色/描边画到底 —— 收起态就是一条干净的圆角把手，
        // 而不是「内容区没了、头部还杵在那儿」。
        const float headH = (float)HeadH();
        const float headFade =
            showTitle_ ? std::clamp((float)(curH_ - kCollapsedHeadH) /
                                        (float)std::max(1, headerH_ - kCollapsedHeadH),
                                    0.0f, 1.0f)
                       : 0.0f;
        const bool drawHead = showTitle_ && headFade > 0.02f;
        const BYTE fadeA = (BYTE)std::lround(255.0f * headFade);
        if (drawHead) {
            GraphicsPath head;
            {
                const float d = (float)rad * 2.0f;
                if (rad <= 0.5) {
                    head.AddRectangle(RectF(0.0f, 0.0f, (float)bufW_, headH));
                } else {
                    head.AddArc(0.0f, 0.0f, d, d, 180.0f, 90.0f);
                    head.AddArc((float)bufW_ - d, 0.0f, d, d, 270.0f, 90.0f);
                    head.AddLine((float)bufW_, headH, 0.0f, headH);
                }
                head.CloseFigure();
            }
            const BYTE hA = hasBackdrop ? (BYTE)std::min(255, (int)(alpha * 0.72))
                                        : (BYTE)std::min(255, alpha + 42);
            SolidBrush headBrush{Color((BYTE)(hA * headFade), (BYTE)Lighten(r, 0.45),
                                        (BYTE)Lighten(g, 0.45), (BYTE)Lighten(b, 0.45))};
            // 头部这个路径是「上圆下方」的，只适合贴在卡片顶上。盒子收起时
            // 卡片只剩头部那么高，它的两个下直角就落到卡片圆角外面去了 ——
            // 表现为头部左右下角「多出两个直角」。按卡片形状裁一刀就干净了。
            gfx.SetClip(&shape);
            gfx.FillPath(&headBrush, &head);
            gfx.ResetClip();
        }

        // ---- 描边 / 分隔线 ----
        Pen border{Color(46, 0, 0, 0), 1.0f};
        gfx.DrawPath(&border, &shape);
        if (drawHead) {
            Pen sep{Color((BYTE)(30 * headFade), 0, 0, 0), 1.0f};
            gfx.DrawLine(&sep, 0.0f, headH, (float)bufW_, headH);
        }

        const ARGB fg = light ? 0xFF1B1B1F : 0xFFF2F2F5;

        // ---- 头部：分组标签 / 单一标题 ----
        // 有分组时整个头部变成一排标签（当前分组高亮）。标签是导航控件，
        // 不受「显示图标标题」开关影响 —— 关掉它标签就没了，等于切不了分组。
        if (drawHead && hasTabs()) {
            const std::vector<RECT> tabs = TabRects();
            const int act = activeGroup();
            FontFamily family(L"Microsoft YaHei UI");
            const int fontPx = (headerH_ >= 40) ? 14 : 13;
            Font tabFont(&family, (float)fontPx, FontStyleRegular, UnitPixel);
            Font tabFontBold(&family, (float)fontPx, FontStyleBold, UnitPixel);

            // ---- 滑块（当前分组身后的那块底色）----
            // 样式参考 Windows 11 的分段控件：浅色卡片上是「更白的胶囊 + 极淡描边
            // + 一点点投影」，深色卡片上是「一层更亮的白」。位置由 TabPillRect()
            // 给出，切换分组时它在两个标签之间补间，不是瞬移。
            const RECT pill = TabPillRect();
            if (pill.right - pill.left >= 4 && pill.bottom - pill.top >= 4) {
                const RectF pr((float)pill.left, (float)pill.top,
                               (float)(pill.right - pill.left), (float)(pill.bottom - pill.top));
                GraphicsPath pp;
                // 圆角 4px：之前 7px 太圆，跟头部的分段控件气质不搭（用户要求改小）
                AddRoundRect(pp, pr, 4.0f);
                if (light) {
                    // 先垫一层淡影：不然纯白块贴在浅色卡片上是「死」的，没有层次
                    GraphicsPath sh;
                    AddRoundRect(sh, RectF(pr.X, pr.Y + 1.0f, pr.Width, pr.Height), 4.0f);
                    SolidBrush shadow{Color((BYTE)(18 * headFade), 0, 0, 0)};
                    gfx.FillPath(&shadow, &sh);
                    SolidBrush face{Color((BYTE)(242 * headFade), 255, 255, 255)};
                    gfx.FillPath(&face, &pp);
                    Pen edge{Color((BYTE)(44 * headFade), 0, 0, 0), 1.0f};
                    gfx.DrawPath(&edge, &pp);
                } else {
                    SolidBrush face{Color((BYTE)(36 * headFade), 255, 255, 255)};
                    gfx.FillPath(&face, &pp);
                    Pen edge{Color((BYTE)(44 * headFade), 255, 255, 255), 1.0f};
                    gfx.DrawPath(&edge, &pp);
                }
            }

            StringFormat tf;
            tf.SetAlignment(StringAlignmentCenter);
            tf.SetLineAlignment(StringAlignmentCenter);
            tf.SetFormatFlags(StringFormatFlagsNoWrap | StringFormatFlagsNoClip);
            tf.SetTrimming(StringTrimmingEllipsisCharacter);
            for (size_t i = 0; i < tabs.size(); ++i) {
                const RECT& tr = tabs[i];
                const bool on = ((int)i == act);
                const RectF rr((float)tr.left, (float)tr.top, (float)(tr.right - tr.left),
                               (float)(tr.bottom - tr.top));
                const BYTE tabA = on ? fadeA : (BYTE)((light ? 122 : 146) * headFade);
                SolidBrush txt{Color(WithAlpha(fg, tabA))};
                const std::wstring name = GroupName((int)i);
                const RectF box(rr.X + 2.0f, rr.Y, std::max(4.0f, rr.Width - 4.0f), rr.Height);
                gfx.DrawString(name.c_str(), -1, on ? &tabFontBold : &tabFont, box, &tf, &txt);
            }
        } else if (drawHead && g_config.showLabels) {
            FontFamily family(L"Microsoft YaHei UI");
            const float fs = headerH_ >= 40 ? 14.0f : 12.5f;
            Font titleFont(&family, fs, FontStyleRegular, UnitPixel);
            SolidBrush brush{Color(WithAlpha(fg, fadeA))};
            StringFormat fmt;
            fmt.SetLineAlignment(StringAlignmentCenter);
            fmt.SetFormatFlags(StringFormatFlagsNoWrap | StringFormatFlagsNoClip);
            fmt.SetTrimming(StringTrimmingEllipsisCharacter);
            gfx.DrawString(cfg_->title.c_str(), -1, &titleFont,
                           RectF(16.0f, 0.0f, (float)bufW_ - 56.0f, headH),
                           &fmt, &brush);
        }

        // ---- 锁按钮（自绘挂锁）----
        // 锁上：实心锁体 + 蓝色；未锁：只有描边、颜色淡一些，不抢标题的视觉重量。
        if (drawHead) {
            const RECT lr = LockRect();
            const float cx = (float)(lr.left + lr.right) * 0.5f;
            const float cy = (float)(lr.top + lr.bottom) * 0.5f;
            const bool on = locked();
            // 白色：和头部标题一致，状态只靠「实心 / 淡」区分（锁上更实、未锁更淡）。
            const Color lc = on ? Color(fadeA, 255, 255, 255)
                                : Color((BYTE)(150 * headFade), 255, 255, 255);
            // 优先用图标字体里的「锁定 / 解锁」（iconfont：e635 / e616）：
            // 手搓的挂锁在 18px 里能表达的状态差异很有限（只有锁梁抬不抬起来），
            // 图标字体的解锁是另一张图，一眼就能分清。
            if (IconFont::Ready()) {
                IconFont::Draw(gfx, on ? IconCode::kLock : IconCode::kUnlock, cx, cy, 15.0f,
                               lc.GetValue());
            } else {
            // 锁体：宽 11、高 9，底边贴着 cy+5
            const RectF body(cx - 5.5f, cy - 1.0f, 11.0f, 9.0f);
            GraphicsPath bp;
            AddRoundRect(bp, body, 2.0f);
            // 锁梁：半圆，锁上时扣在锁体上（低一点、闭合），未锁时抬高一点
            GraphicsPath sp;
            const float lift = on ? 0.0f : -1.5f;
            sp.AddArc(cx - 4.0f, cy - 7.5f + lift, 8.0f, 8.0f, 180.0f, 180.0f);
            Pen shackle(lc, 1.5f);
            shackle.SetStartCap(LineCapRound);
            shackle.SetEndCap(LineCapRound);
            gfx.DrawPath(&shackle, &sp);
            if (on) {
                SolidBrush fill(lc);
                gfx.FillPath(&fill, &bp);
            } else {
                Pen pen(lc, 1.3f);
                gfx.DrawPath(&pen, &bp);
            }
            }
        }

        // ---- 展开方式箭头（点击在两种模式之间切）----
        // 同一个 V 形，只转 180°：
        //   展开模式（始终展开）→ 朝下的 V：躺开不动
        //   伸缩模式（悬停展开）→ 朝上的 ^：鼠标一离开就会收上去
        // 收起态整条把手只有 4px，头部内容全淡出，不画箭头（headFade 会把它抹掉）。
        if (drawHead) {
            const float ang = (expandMode() == 0) ? (float)3.14159265358979f : 0.0f;

            Pen pen{Color(WithAlpha(fg, fadeA)), 1.8f};
            pen.SetStartCap(LineCapRound);
            pen.SetEndCap(LineCapRound);
            const float cx = (float)bufW_ - 21.0f;
            const float cy = headH * 0.5f;
            const float ca = std::cos(ang), sa = std::sin(ang);
            auto rot = [&](float x, float y) {
                return PointF(cx + x * ca - y * sa, cy + x * sa + y * ca);
            };
            // 未旋转时是「朝下的 V」；转 180° 就是「朝上的 ^」
            const PointF a = rot(-5.0f, -2.5f);
            const PointF b = rot(0.0f, 2.5f);
            const PointF c = rot(5.0f, -2.5f);
            gfx.DrawLine(&pen, a, b);
            gfx.DrawLine(&pen, b, c);
        }

        // ---- 悬停高亮（拖动中不画：被拖的那个已变成"空位"，再套一层高亮很乱）----
        // 直角填充：盒内图标的底色和桌面图标保持一致，不做圆角
        // （逐像素 alpha 的圆角在这个尺寸下没有抗锯齿，边缘会明显发毛）。
        if (!collapsed_ && hot_ >= 0 && !itemDragging_) {
            RECT ir = ItemHighlightRect(hot_);
            if (ir.right > ir.left) {
                SolidBrush hl{light ? Color(26, 0, 0, 0) : Color(52, 255, 255, 255)};
                gfx.FillRectangle(&hl, Rect(ir.left + 2, ir.top + 2, ir.right - ir.left - 4,
                                            cellH() - 4));
            }
        }

        // ---- 滚动条：不画 ----
        // 内容超出可视区时原来会在右边缘画一根 3px 的细条。用户要求去掉：
        // 盒子里图标多的时候，那根条正好压在最后一列图标上，很碍眼。
        // 滚动本身完全保留（鼠标滚轮照旧），只是不再有可见指示器 ——
        // 判断「下面还有没有」靠的是图标被切一半的视觉效果，足够了。

        // ---- 选中高亮（支持多选；双击才打开，所以要有可见的选中态）----
        if (!collapsed_ && !itemDragging_) {
            std::vector<DesktopIconView*> mineSel;
            CollectVisible(mineSel);
            SolidBrush fill{light ? Color(34, 0, 0, 0) : Color(44, 255, 255, 255)};
            Pen pen{light ? Color(70, 0, 0, 0) : Color(80, 255, 255, 255), 1.0f};
            for (size_t i = 0; i < mineSel.size(); ++i) {
                if (!mineSel[i] || !mineSel[i]->selected) continue;
                RECT sr = ItemHighlightRect((int)i);
                if (sr.right <= sr.left) continue;
                const Rect rc(sr.left + 3, sr.top + 3, sr.right - sr.left - 6, cellH() - 6);
                // 浅色卡片上用暗一点的高亮，深色卡片上用亮一点的
                gfx.FillRectangle(&fill, rc);
                gfx.DrawRectangle(&pen, rc);
            }
        }

        // ---- 搜索「定位」的闪烁：只画亮相（0/2），灭相什么都不画 ----
        if (!collapsed_ && flash_ && (flashStep_ & 1) == 0) {
            std::vector<DesktopIconView*> mineFlash;
            CollectVisible(mineFlash);
            for (size_t i = 0; i < mineFlash.size(); ++i) {
                if (mineFlash[i] != flash_) continue;
                int ox = 0, oy = 0;
                if (!ItemOrigin((int)i, ox, oy)) break;   // 滚出视野了就别画
                RECT fr = ItemHighlightRect((int)i);
                if (fr.right <= fr.left) break;
                const Rect rc(fr.left + 3, fr.top + 3, fr.right - fr.left - 6, cellH() - 6);
                SolidBrush ff{Color(84, 0, 120, 215)};
                gfx.FillRectangle(&ff, rc);
                Pen fp{Color(255, 0, 120, 215), 2.0f};
                gfx.DrawRectangle(&fp, rc);
                break;
            }
        }

        // ---- 框选矩形（和桌面同一个观感：淡蓝填充 + 1px 实线）----
        if (marquee_) {
            const int l = std::min(marqueeA_.x, marqueeB_.x);
            const int t = std::min(marqueeA_.y, marqueeB_.y);
            const int r = std::max(marqueeA_.x, marqueeB_.x);
            const int b = std::max(marqueeA_.y, marqueeB_.y);
            if (r - l > 1 && b - t > 1) {
                SolidBrush mf{Color(38, 0, 120, 215)};
                gfx.FillRectangle(&mf, Rect(l, t, r - l, b - t));
                Pen mp{Color(255, 0, 120, 215), 1.0f};
                gfx.DrawRectangle(&mp, Rect(l, t, r - l - 1, b - t - 1));
            }
        }

        // ---- 拖拽落点：中间区域的高亮（底色，画在图标下面才有"整格选中"的感觉）----
        if (!collapsed_ && drop_.item >= 0 && drop_.zone == 0) {
            RECT ir = ItemHighlightRect(drop_.item);
            if (ir.right > ir.left) {
                const Rect rc(ir.left + 3, ir.top + 3, ir.right - ir.left - 6, cellH() - 6);
                SolidBrush fill{Color(58, 0, 120, 215)};
                gfx.FillRectangle(&fill, rc);
                Pen pen{Color(255, 0, 120, 215), 1.6f};
                gfx.DrawRectangle(&pen, rc);
            }
        }

        // ---- 空状态提示 ----
        std::vector<DesktopIconView*> mineEmpty;
        CollectVisible(mineEmpty);
        if (!collapsed_ && mineEmpty.empty()) {
            FontFamily family(L"Microsoft YaHei UI");
            Font hintFont(&family, 12.0f, FontStyleRegular, UnitPixel);
            SolidBrush h2{Color((BYTE)(light ? 150 : 170), (BYTE)(light ? 90 : 210),
                                 (BYTE)(light ? 96 : 210), (BYTE)(light ? 108 : 210))};
            StringFormat hf;
            hf.SetAlignment(StringAlignmentCenter);
            hf.SetLineAlignment(StringAlignmentCenter);
            gfx.DrawString(L"\u628a\u684c\u9762\u56fe\u6807\u62d6\u8fdb\u6765", -1, &hintFont,
                           RectF(0.0f, headH, (float)bufW_, (float)bufH_ - headH),
                           &hf, &h2);
        }

        gfx.Flush(FlushIntentionSync);
    }

    // 本帧每个图标画在哪（客户区像素，可插值）。
    // 改排序时从「旧槽位」平滑滑到「新槽位」——用户要的是位移过渡，不是原地翻转。
    std::vector<DesktopIconView*> mine;
    CollectVisible(mine);
    std::vector<POINT> posOf(mine.size(), POINT{0, 0});
    std::vector<bool> posOk(mine.size(), false);
    std::vector<int> dxOf(mine.size(), 0);   // 相对目标槽位的横向偏移（标签跟着走）
    {
        const float raw = FlipProgress();
        // easeInOutCubic：起步和收尾都柔，中间快，位移感最像「被推过去」
        const float t = (raw < 0.5f) ? 4.0f * raw * raw * raw
                                     : 1.0f - std::pow(-2.0f * raw + 2.0f, 3.0f) / 2.0f;
        for (size_t i = 0; i < mine.size(); ++i) {
            int tx = 0, ty = 0;
            if (!ItemOrigin((int)i, tx, ty)) continue;
            posOk[i] = true;
            posOf[i] = POINT{tx, ty};
            if (!flipActive_) continue;
            auto f = flipNodes_.find(mine[i]);
            if (f == flipNodes_.end()) continue;
            int fx = 0, fy = 0;
            if (!ItemOrigin(f->second.from, fx, fy)) continue;
            posOf[i].x = (int)std::lround(fx + (tx - fx) * t);
            posOf[i].y = (int)std::lround(fy + (ty - fy) * t);
            dxOf[i] = posOf[i].x - tx;
        }
    }

    // ---- 图标（手工预乘合成，1:1 无重采样）+ 标题 ----
    if (!collapsed_) {
        auto* dst = reinterpret_cast<BYTE*>(bits_);
        const int ch = cellH();
        const bool listMode = IsListView();
        const bool detailMode = viewMode() == 5;
        const int glow = g_config.textShadow ? 2 : 0;
        const int textH = LabelTextHeight();

        for (size_t i = 0; i < mine.size(); ++i) {
            DesktopIconView* e = mine[i];
            if (!posOk[i]) continue;
            const int ix = posOf[i].x;
            const int iy = posOf[i].y;

            // 正在被拖的（多选时是整批）留在原位当「空位」：变淡 + 变暗，松手后才重新排位
            const bool ghost =
                itemDragging_ &&
                std::find(dragItems_.begin(), dragItems_.end(), e) != dragItems_.end();
            const int ga = ghost ? 110 : 255;   // 图标不透明度
            const int gd = ghost ? 120 : 255;   // 额外压暗
            const int gaLabel = ga;

            const IconBitmap& bm = e->bmp;
            const int iconW = bm.valid() ? bm.width : 32;
            const int iconH = bm.valid() ? bm.height : 32;

            // 列表 / 详情：图标靠左、文字在右边一行；其它模式：图标居中、文字在下方
            DetailCols dl{};
            int iconX = 0;
            int iconY = 0;
            if (detailMode) {
                dl = DetailLayoutFor(kPad, width_ - kPad * 2);
                iconX = dl.iconX;
                iconY = iy + (ch - iconH) / 2;
            } else if (listMode) {
                iconX = ix + 8;
                iconY = iy + (ch - iconH) / 2;
            } else {
                // ItemOrigin 已经给出「以列中心居中」后的图标左上角
                iconX = ix + (iconPx() - iconW) / 2;
                iconY = iy + (showLabels_ ? 4 : (ch - iconH) / 2);
            }

            if (bm.valid()) {
                BlendIntoDim(dst, bufW_, bufH_, bm.pixels.data(), bm.width, bm.height,
                             iconX, iconY, ga, gd);
            }

            // 正在原地改名：名字由输入框代替（见 DesktopIconView::renaming）
            if (e->name.empty() || e->renaming) continue;

            // 和桌面图标标题保持一致：白字 + 柔和黑光晕，光晕受「文字阴影」开关控制
            if (detailMode) {
                // 详情：名称是表格里的一列（不是图标下方标签），左对齐、单行省略
                if (dl.nameW <= 0) continue;
                LabelDib* lab = DetailText(e->name, dl.nameW, textH, glow);
                if (lab && lab->valid()) {
                    BlendIntoDim(dst, bufW_, bufH_,
                                 reinterpret_cast<const BYTE*>(lab->bits), lab->w, lab->h,
                                 dl.nameX - lab->pad, iy + (ch - textH) / 2 - lab->pad,
                                 gaLabel, gd);
                }
            } else if (showLabels_) {
                // 平时一行（超出省略），单击选中后展开成两行 —— 和桌面图标一致
                const int maxLines = e->selected ? 2 : 1;
                if (listMode) {
                    const int labW = std::max(24, (width_ - kPad * 2) - (iconX - ix) - iconW - 8);
                    // 列表行的标签盒给到「两行」的高度（图标本身就有 32px，够两行）
                    int labH = iconH;
                    const int twoLine = LabelTextHeight() * 2;
                    if (labH < twoLine) labH = std::min(twoLine, ch - 2);
                    LabelDib* lab = LabelOf(e, labW, labH, glow, maxLines);
                    if (lab && lab->valid()) {
                        const int left = iconX + iconW + 8 - lab->pad;
                        const int top = iy + (ch - textH) / 2 - lab->pad;
                        BlendIntoDim(dst, bufW_, bufH_,
                                     reinterpret_cast<const BYTE*>(lab->bits), lab->w, lab->h,
                                     left, top, gaLabel, gd);
                    }
                } else {
                    const int iconBottom = iy + 4 + iconH;
                    const int textTop = iconBottom + 2;
                    int th = ch - (textTop - iy);
                    // 标题区最多两行：格子高度就是按两行预留的，
                    // 这里再夹一次，免得「不显示标题再打开」之类的老配置把第三行画出来
                    const int twoLine = LabelTextHeight() * 2;
                    if (th > twoLine) th = twoLine;
                    if (th < 14) th = 14;
                    int labLeft = 0, labW = 0;
                    ItemLabelBox((int)i, labLeft, labW);
                    labLeft += dxOf[i];   // 排序过渡中文字跟着图标一起滑
                    LabelDib* lab = LabelOf(e, labW, th, glow, maxLines);
                    if (lab && lab->valid()) {
                        int contentW = lab->w - lab->pad * 2;
                        int left = labLeft + (labW - contentW) / 2 - lab->pad;
                        BlendIntoDim(dst, bufW_, bufH_,
                                     reinterpret_cast<const BYTE*>(lab->bits), lab->w, lab->h,
                                     left, textTop - lab->pad, gaLabel, gd);
                    }
                }
            }
        }
    }

    // ---- 详情模式：名称右侧的「修改日期 / 类型 / 大小」三列 ----
    // 全部走和图标标签同一套 GDI 栅格化（清晰），按盒子宽度逐列隐藏。
    if (!collapsed_ && viewMode() == 5) {
        if (detailInfo_.size() != mine.size()) BuildDetailInfo();
        if (!detailInfo_.empty()) {
            auto* dst = reinterpret_cast<BYTE*>(bits_);
            const int ch = cellH();
            const int glow = g_config.textShadow ? 2 : 0;
            const int textH = LabelTextHeight();

            for (size_t i = 0; i < mine.size(); ++i) {
                if (!posOk[i]) continue;
                const int iy = posOf[i].y;
                auto d = detailInfo_.find(mine[i]);
                if (d == detailInfo_.end()) continue;

                const DetailCols dl = DetailLayoutFor(kPad, width_ - kPad * 2);
                const bool ghost = itemDragging_ && mine[i] == dragged_;
                const int ga = ghost ? 110 : 255;

                const int top = iy + (ch - textH) / 2;
                auto drawCol = [&](const std::wstring& text, int cx, int cw) {
                    if (cw <= 0 || text.empty()) return;
                    LabelDib* lab = DetailText(text, cw, textH, glow);
                    if (!lab || !lab->valid()) return;
                    // 右对齐：文字贴列右边界
                    const int contentW = lab->w - lab->pad * 2;
                    const int left = cx + cw - contentW - lab->pad;
                    BlendIntoDim(dst, bufW_, bufH_,
                                 reinterpret_cast<const BYTE*>(lab->bits), lab->w, lab->h,
                                 left, top - lab->pad, ga, ghost ? 120 : 255);
                };
                if (dl.showDate) drawCol(d->second.date, dl.dateX, dl.dateW);
                if (dl.showType) drawCol(d->second.type, dl.typeX, dl.typeW);
                if (dl.showSize) drawCol(d->second.size, dl.sizeX, dl.sizeW);
            }
        }
    }

    // ---- 拖拽插入缝隙线：必须画在图标「上面」----
    // 以前它跟其它高亮一起画在图标之前，落点位置恰好是图标的边缘，结果整条线被
    // 图标盖住 —— 表现就是「拖到盒子里左右不再出现缝隙线」。
    if (!collapsed_ && drop_.item >= 0 && drop_.zone != 0) {
        const int cols = CellCols();
        const int c = drop_.item % cols;
        const int row = drop_.item / cols;
        const int ch = cellH();
        const int top = HeadH() + kPad + row * ch - scrollY_;
        const int iw = iconPx();
        const int cxi = ColCenterX(c, cols);
        // 线落在两个图标之间的空隙中线：图标边缘往外半个空隙
        int gap = cellW_;
        if (cols > 1) gap = ColCenterX(1, cols) - ColCenterX(0, cols);
        gap = std::max(6, (gap - iw) / 2);
        float lx = (drop_.zone < 0) ? (float)(cxi - iw / 2 - gap)
                                    : (float)(cxi + iw / 2 + gap);
        lx -= 1.5f;   // 线宽 3px，往左挪半条让它落在中线上
        if (lx < 2.0f) lx = 2.0f;
        if (lx > (float)width_ - 5.0f) lx = (float)width_ - 5.0f;

        Bitmap bmpLine(bufW_, bufH_, bufW_ * 4, PixelFormat32bppPARGB,
                       reinterpret_cast<BYTE*>(bits_));
        Graphics g2(&bmpLine);
        g2.SetSmoothingMode(SmoothingModeAntiAlias);
        g2.SetPixelOffsetMode(PixelOffsetModeHighQuality);
        GraphicsPath bar;
        AddRoundRect(bar, RectF(lx, (float)top + 6.0f, 3.0f, (float)ch - 12.0f), 1.5f);
        SolidBrush ab{Color(255, 0, 120, 215)};
        g2.FillPath(&ab, &bar);
        g2.Flush(FlushIntentionSync);
    }
}

LabelDib* FenceWindow::LabelOf(DesktopIconView* e, int boxW, int boxH, int glow, int maxLines) {
    if (maxLines < 1) maxLines = 1;
    // maxLines 必须进 key：选中/取消选中会切换单行↔两行，不进 key 就会一直用旧的
    std::wstring key = e->name + L"\x1|" + std::to_wstring(boxW) + L"x" +
                       std::to_wstring(boxH) + L"g" + std::to_wstring(glow) + L"m" +
                       std::to_wstring(maxLines);
    auto k = labelKeys_.find(e);
    if (k != labelKeys_.end() && k->second == key) {
        auto it = labels_.find(e);
        if (it != labels_.end()) return &it->second;
    }
    LabelDib d;
    // 与桌面同一套参数：白色 + 光晕
    if (!d.Build(e->name, IconLabelFont(), boxW, boxH, glow, RGB(255, 255, 255), maxLines)) {
        labelKeys_[e] = key;
        labels_[e] = LabelDib{};
        return nullptr;
    }
    labelKeys_[e] = key;
    auto r = labels_.insert_or_assign(e, std::move(d));
    return &r.first->second;
}

void FenceWindow::Present() {
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

void FenceWindow::Redraw() {
    if (!hwnd_) return;
    // 图标数量 / 尺寸 / 盒子高度都可能刚变过，滚动位置可能已经越界
    ClampScroll();
    EnsureBackbuffer(width_, curH_);
    Paint();
    Present();
}

void FenceWindow::RecaptureBackdrop() {
    if (!cfg_ || !cfg_->acrylic || !hwnd_) return;
    bdJob_.reset();       // 在途的那一单作废（换壁纸了，抓到的是旧壁纸）
    bdPlainArea_ = false; // 壁纸变了，「这块是纯色」的结论也要重来
    // 拖动 / 改尺寸途中就再抓一圈大的，别把正在用的那一圈收窄了
    RefreshBackdrop((dragging_ || resizing_) ? 128 : 0);
    Redraw();
}

void FenceWindow::ScheduleBackdropRefresh() {
    if (!hwnd_) return;
    bdRefreshPending_ = true;
    // 延后 260ms 再抓：SPI_SETDESKWALLPAPER 这条广播比画面早到，收到就抓
    // 抓到的还是旧壁纸（DWM 还没合成新的），然后就没有第二次机会了 ——
    // 磨砂就一直停在旧壁纸上。这正是「壁纸变了盒子不变」的另一半原因。
    bdDue_ = GetTickCount() + 260;
}

// 壁纸是否变了 —— 靠「盒子外面一小块屏幕的平均色」当指纹。
//
// 为什么不能只等广播：动态壁纸 / 幻灯片轮播根本不会发 SPI_SETDESKWALLPAPER，
// 只有像素真的变了才是唯一可靠的信号。
//
// 为什么采样点必须在盒子外面：盒子自己盖住的地方抓到的就是我们自己画的画面，
// 拿它当指纹等于自己跟自己比，永远比不出结果。
void FenceWindow::PollBackdropChange() {
    if (!hwnd_ || !cfg_ || !cfg_->acrylic) return;
    if (dragging_ || resizing_ || bdJob_) return;   // 忙的时候不插队（拖动时绝不抓屏）

    const DWORD now = GetTickCount();
    if (bdRefreshPending_) {
        if ((int)(now - bdDue_) < 0) return;   // 还没到点
        bdRefreshPending_ = false;
        probeValid_ = false;   // 重抓完重新立基线
        probeHits_ = 0;
        RecaptureBackdrop();
        return;
    }

    RECT r = rect();
    const int w = (int)(r.right - r.left);
    const int h = (int)(r.bottom - r.top);
    if (w <= 0 || h <= 0) return;

    const int vsx = GetSystemMetrics(SM_XVIRTUALSCREEN);
    const int vsy = GetSystemMetrics(SM_YVIRTUALSCREEN);
    const int vsw = GetSystemMetrics(SM_CXVIRTUALSCREEN);
    const int vsh = GetSystemMetrics(SM_CYVIRTUALSCREEN);
    const int kProbe = 16;   // 采样块大小
    const int kGap = 6;      // 离盒子多远（贴着边缘会被描边/阴影算进去）

    // 下 / 上 / 右 / 左依次试，取第一个整块落在虚拟桌面里的
    const int cx = (int)r.left + w / 2;
    const int cy = (int)r.top + h / 2;
    const int candX[4] = {cx, cx, (int)r.right + kGap, (int)r.left - kGap - kProbe};
    const int candY[4] = {(int)r.bottom + kGap, (int)r.top - kGap - kProbe, cy, cy};
    int px = 0, py = 0;
    bool found = false;
    for (int i = 0; i < 4 && !found; ++i) {
        if (candX[i] >= vsx && candY[i] >= vsy && candX[i] + kProbe <= vsx + vsw &&
            candY[i] + kProbe <= vsy + vsh) {
            px = candX[i];
            py = candY[i];
            found = true;
        }
    }
    if (!found) return;   // 盒子把屏幕铺满了，没地方采样（这时也不用跟了）

    int rr = 0, gg = 0, bb = 0;
    if (!SampleScreenMean(px, py, kProbe, kProbe, rr, gg, bb)) return;

    if (!probeValid_) {
        probeValid_ = true;
        probeR_ = rr;
        probeG_ = gg;
        probeB_ = bb;
        return;
    }
    const int diff = std::max({std::abs(rr - probeR_), std::abs(gg - probeG_),
                               std::abs(bb - probeB_)});
    if (diff < 8) {
        probeHits_ = 0;
        return;
    }
    // 变了。要求连续两次都变才动手：别的窗口从采样点上面扫过去、或者那附近
    // 有个桌面图标被 hover 出了一层底色，均值同样会抖几个数 —— 一次就信会误抓。
    if (++probeHits_ < 2) return;
    probeHits_ = 0;
    probeR_ = rr;
    probeG_ = gg;
    probeB_ = bb;
    // 走后台抓（不是同步 RecaptureBackdrop）：壁纸在动的时候这会每隔一会儿
    // 来一次，同步抓就是十几毫秒的卡顿 —— 玩游戏时最不能要的就是这个。
    RequestBackdropAsync(0);
}

void FenceWindow::ApplyConfig(bool recapture) {
    if (!hwnd_ || !cfg_) return;

    const int newHeader = std::clamp(g_config.headerHeight, 26, 50);
    const bool newLabels = g_config.showLabels != 0;
    const bool newTitle = g_config.showFenceTitle != 0;
    if (newHeader != headerH_ || newLabels != showLabels_ || newTitle != showTitle_) {
        headerH_ = newHeader;
        showLabels_ = newLabels;
        showTitle_ = newTitle;
        labels_.clear();
        labelKeys_.clear();
        // 收起高度跟着走：收起态整窗只有 4px（kCollapsedHeadH），和头部高度无关
        if (collapsed_ && showTitle_) {
            targetH_ = kCollapsedHeadH;
            curH_ = kCollapsedHeadH;
            RECT r = rect();
            SetWindowPos(hwnd_, nullptr, r.left, r.top, width_, curH_,
                         SWP_NOZORDER | SWP_NOACTIVATE);
        } else if (collapsed_) {
            // 关掉「显示盒子标题」之后没有箭头了，收起的盒子就再也打不开 ——
            // 顺手展开（expandMode() 在这种情况下恒为「始终展开」）
            collapsed_ = false;
            targetH_ = height_;
            curH_ = height_;
            RECT r = rect();
            SetWindowPos(hwnd_, nullptr, r.left, r.top, width_, curH_,
                         SWP_NOZORDER | SWP_NOACTIVATE);
        }
    }

    if (recapture) RefreshBackdrop();

    // 「查看」可能在外部（右键菜单 / 配置）被改过，格子宽度跟着走
    const int newCellW = CellWForMode(cfg_->viewMode);
    if (newCellW != cellW_) {
        cellW_ = newCellW;
        labels_.clear();
        labelKeys_.clear();
        ClearDetailCache();
    }
    Redraw();
}

bool FenceWindow::Create(int index, int x, int y, int w, int h) {
    EnsureGdiplus();

    index_ = index;
    if (index >= 0 && index < (int)g_config.fences.size()) cfg_ = &g_config.fences[index];
    if (!cfg_) return false;

    HINSTANCE inst = GetModuleHandleW(nullptr);
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.style = CS_DBLCLKS;   // 双击图标才打开，没这个收不到 WM_LBUTTONDBLCLK
    wc.lpfnWndProc = WndProcStatic;
    wc.hInstance = inst;
    wc.lpszClassName = kClass;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    RegisterClassExW(&wc);

    width_ = w;
    height_ = h;
    headerH_ = std::clamp(g_config.headerHeight, 26, 50);
    showTitle_ = g_config.showFenceTitle != 0;
    showLabels_ = g_config.showLabels != 0;
    cellW_ = CellWForMode(cfg_->viewMode);
    curH_ = h;
    targetH_ = h;

    // 调试口子：MEOW_FLIP_MS=1200 可以把排序 flip 动画放慢，方便逐帧截图核对
    {
        wchar_t v[32]{};
        if (GetEnvironmentVariableW(L"MEOW_FLIP_MS", v, 32) > 0) {
            const int n = _wtoi(v);
            if (n > 0) flipDur_ = n;
        }
        if (GetEnvironmentVariableW(L"MEOW_DEBUG_HOOKS", v, 32) > 0 && _wtoi(v) != 0)
            debugHooks_ = true;
    }

    hwnd_ = CreateWindowExW(WS_EX_LAYERED | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
                            kClass, L"meow-desk-fence", WS_POPUP,
                            x, y, w, h, nullptr, nullptr, inst, this);
    if (!hwnd_) return false;

    EnsureBackbuffer(width_, curH_);
    RefreshBackdrop();
    Paint();
    Present();
    ShowWindow(hwnd_, SW_SHOWNOACTIVATE);
    return true;
}

void FenceWindow::SetCollapsed(bool collapsed) {
    if (collapsed_ == collapsed) return;
    collapsed_ = collapsed;
    hot_ = -1;
    StopFlip();
    drop_ = DropHint{};
    // 收起 = 整块缩成一条 4px 的把手（头部一起收，不只是内容区）。
    // 以前这里是 HeadH()（只把内容区收掉，头部保持原样），用户要求头部也收。
    targetH_ = collapsed ? kCollapsedHeadH : height_;
    if (hwnd_) SetTimer(hwnd_, kAnimTimer, 16, nullptr);
    Redraw();
}

void FenceWindow::SetExpandMode(int m) {
    if (!cfg_) return;
    const int v = m == 0 ? 0 : 1;
    if (cfg_->expandMode == v) return;
    cfg_->expandMode = v;
    // 切到「始终展开」就立刻摊开；切到「伸缩模式」不在这里收 ——
    // 是让鼠标离开时自然收（箭头那一下会自己 SetCollapsed(true) 给个即时反馈）
    if (v == 1) SetCollapsed(false);
    Redraw();
    if (onStateChanged) onStateChanged();   // 存配置
}

void FenceWindow::Destroy() {
    if (hwnd_) {
        FileDrop::Detach(hwnd_);   // OLE 那边还挂着这个 HWND，先摘掉再销毁
        KillTimer(hwnd_, kAnimTimer);
        KillTimer(hwnd_, kFlashTimer);
        flash_ = nullptr;
        locatePending_ = nullptr;
        SetWindowLongPtrW(hwnd_, GWLP_USERDATA, 0);
        DestroyWindow(hwnd_);
        hwnd_ = nullptr;
    }
    if (dib_) { DeleteObject(dib_); dib_ = nullptr; }
    if (memDC_) { DeleteDC(memDC_); memDC_ = nullptr; }
    bits_ = nullptr;
    bufW_ = bufH_ = 0;
    bdJob_.reset();   // 工作线程那一单由 shared_ptr 自己收尾，不会悬空
    pendingSlots_.clear();   // 里面是指向图标的裸指针，盒子拆了就不能留
    backdrop_.clear();
    labels_.clear();
    labelKeys_.clear();
    detailInfo_.clear();
    detailText_.clear();
    StopFlip();
}

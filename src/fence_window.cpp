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
#include "icon_render.h"

#pragma comment(lib, "gdiplus.lib")
#pragma comment(lib, "shlwapi.lib")

using namespace Gdiplus;

namespace {

const wchar_t* kClass = L"MeowDeskFence";

const int kPad = 14;
const int kCellWDefault = 74;

// 四条边/四角可以拖拽改大小的判定宽度
const int kEdgeMargin = 6;
const UINT_PTR kAnimTimer = 2;

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

// 标签字体的行高：LabelDib 里 DrawTextW 是 DT_TOP，所以盒子高度给到行高、
// 再按 (行高 - 行高)/2 自然就垂直居中了。量一次缓存起来。
int LabelTextHeight() {
    static int cached = 0;
    if (cached > 0) return cached;
    HDC dc = CreateCompatibleDC(nullptr);
    if (!dc) return 16;
    HGDIOBJ of = SelectObject(dc, IconLabelFont());
    TEXTMETRICW tm{};
    GetTextMetricsW(dc, &tm);
    SelectObject(dc, of);
    DeleteDC(dc);
    cached = tm.tmHeight > 0 ? (int)tm.tmHeight : 16;
    return cached;
}

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

void FenceWindow::Collect(std::vector<DesktopIconView*>& out) const {
    out.clear();
    if (!all_) return;
    for (auto* e : *all_) {
        if (e && e->owner == index_) out.push_back(e);
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
// 第 0 列图标左边缘贴 kPad，最后一列右边缘贴 width_-kPad，中间等步长；
// 所以每一排满的时候两端都顶到内边距，不满的最后一排从 kPad 起左对齐，
// 而且列位置和上一排严丝合缝地对齐（步长相同）。
int FenceWindow::ColCenterX(int c, int cols) const {
    const int iw = iconPx();
    if (cols <= 1) return kPad + iw / 2;
    if (c <= 0) return kPad + iw / 2;
    if (c >= cols) return width_ - kPad - iw / 2;
    const int usable = width_ - kPad * 2;
    const int span = usable - iw;   // 首列中心 → 末列中心
    return kPad + iw / 2 + (int)std::lround((double)span * c / (cols - 1));
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
    const int top = headerH_ + kPad + row * ch;

    if (IsListView()) {
        r = RECT{kPad, top, width_ - kPad, top + ch};
        return;
    }
    const int cx = ColCenterX(c, cols);
    const int left = (c == 0) ? kPad : (ColCenterX(c - 1, cols) + cx) / 2;
    const int right = (c == cols - 1) ? (width_ - kPad) : (cx + ColCenterX(c + 1, cols)) / 2;
    r = RECT{left, top, right, top + ch};
}

void FenceWindow::ItemLabelBox(int index, int& left, int& w) const {
    const int cols = CellCols();
    const int c = (index < 0) ? 0 : (index % cols);
    const int usable = width_ - kPad * 2;
    if (cols <= 1) {
        w = std::min(cellW_ - 8, usable);
    } else {
        // 步长要扣掉 6px，相邻两列的文字盒才不会贴到一起
        const double step = (double)(usable - iconPx()) / (cols - 1);
        w = std::min(cellW_ - 8, std::max(24, (int)step - 6));
    }
    const int cx = ColCenterX(c, cols);
    left = cx - w / 2;
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
    return headerH_ + 64;       // 头部 + 一行图标的空间
}

// 命中四条边/四个角（客户区坐标）。收起状态下不参与，避免和「拖头部移动」打架。
FenceWindow::Edge FenceWindow::HitEdge(POINT pt) const {
    if (collapsed_) return Edge::None;
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
    dragging_ = false;
    itemDragging_ = false;
    resizing_ = false;
    resizeEdge_ = Edge::None;
    pressItem_ = -1;
    pressArrow_ = false;
    pressHeader_ = false;
    pressBody_ = false;

    // 先判边缘：边缘优先于头部拖动/图标
    const Edge e = HitEdge(pt);
    if (e != Edge::None) {
        resizing_ = true;
        resizeEdge_ = e;
        resizeStartRect_ = dragStartRect_;
        resizeStartW_ = width_;
        resizeStartH_ = height_;
        SetCapture(h);
        return;
    }

    if (pt.y < headerH_ && pt.x > width_ - 40) {
        pressArrow_ = true;
    } else if (pt.y < headerH_) {
        pressHeader_ = true;
    } else if (!collapsed_) {
        pressBody_ = true;
        pressItem_ = HitItem(pt);
    }
    SetCapture(h);
}

void FenceWindow::ApplyResize(POINT screenPt) {
    if (!resizing_ || !hwnd_) return;
    const int dx = screenPt.x - pressScreenPt_.x;
    const int dy = screenPt.y - pressScreenPt_.y;

    int l = resizeStartRect_.left, t = resizeStartRect_.top;
    int r = resizeStartRect_.right, b = resizeStartRect_.bottom;
    const Edge e = resizeEdge_;

    if (e == Edge::L || e == Edge::TL || e == Edge::BL) l += dx;
    if (e == Edge::R || e == Edge::TR || e == Edge::BR) r += dx;
    if (e == Edge::T || e == Edge::TL || e == Edge::TR) t += dy;
    if (e == Edge::B || e == Edge::BL || e == Edge::BR) b += dy;

    // 最小尺寸（从左/上拖时调的是 left/top）
    if (r - l < minW()) {
        if (e == Edge::L || e == Edge::TL || e == Edge::BL) l = r - minW();
        else r = l + minW();
    }
    if (b - t < minH()) {
        if (e == Edge::T || e == Edge::TL || e == Edge::TR) t = b - minH();
        else b = t + minH();
    }

    // 不出屏幕
    MONITORINFO mi{};
    mi.cbSize = sizeof(mi);
    RECT lim{0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN)};
    if (GetMonitorInfoW(MonitorFromWindow(hwnd_, MONITOR_DEFAULTTONEAREST), &mi)) lim = mi.rcWork;
    if (l < lim.left) { if (e == Edge::L || e == Edge::TL || e == Edge::BL) { l = lim.left; } }
    if (t < lim.top) { if (e == Edge::T || e == Edge::TL || e == Edge::TR) { t = lim.top; } }
    if (r > lim.right) { if (e == Edge::R || e == Edge::TR || e == Edge::BR) { r = lim.right; } }
    if (b > lim.bottom) { if (e == Edge::B || e == Edge::BL || e == Edge::BR) { b = lim.bottom; } }
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
    sel_ = -1;
    hot_ = -1;
    if (nw != 0) {
        labels_.clear();
        labelKeys_.clear();
        ClearDetailCache();
    }
    StopFlip();
    Redraw();
}

// ---------------------------------------------------------------------------
// 查看 / 排序（右键菜单驱动）
// ---------------------------------------------------------------------------
int FenceWindow::cellH() const {
    switch (viewMode()) {
    case 0: return showLabels_ ? 64 : 50;     // 小图标
    case 2: return showLabels_ ? 102 : 82;    // 大图标
    case 3: return showLabels_ ? 126 : 102;   // 超大图标
    case 4: return showLabels_ ? 52 : 44;     // 列表（图标 32px，一行一个）
    case 5: return 44;                        // 详情：固定行高，名称在右侧列里
    default: return showLabels_ ? 84 : kCellHCompact;   // 中等图标
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
    sel_ = -1;
    hot_ = -1;
    Redraw();
    if (onStateChanged) onStateChanged();
}

// 详情行：读一次文件属性，把「日期 / 类型 / 大小」三列算好缓存。
// 盒子里的图标一般十几个，一次 IO 完全可以接受；渲染时不再碰磁盘。
void FenceWindow::BuildDetailInfo() {
    detailInfo_.clear();
    if (!all_) return;
    for (auto* e : *all_) {
        if (!e || e->owner != index_) continue;

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

    sel_ = -1;
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
    if (!g_config.sortAnim || collapsed_) return;
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

float FenceWindow::FlipProgress() const {
    if (!flipActive_ || flipDur_ <= 0) return 1.0f;
    const DWORD el = GetTickCount() - flipT0_;
    if (el >= (DWORD)flipDur_) return 1.0f;
    return (float)el / (float)flipDur_;
}

void FenceWindow::SetTitle(const std::wstring& t) {
    if (!cfg_) return;
    cfg_->title = t;
    Redraw();
    if (onStateChanged) onStateChanged();
}

std::wstring FenceWindow::title() const { return cfg_ ? cfg_->title : std::wstring(); }

// 重排本盒子图标在全局表里的相对顺序。
// 只动 owner == index_ 的那些槽位，盒子里第 k 个图标因此变成第 k 个排序结果 ——
// 布局完全由「表里的相对顺序」决定，所以排完不用通知任何人。
void FenceWindow::SortNow() {
    if (!all_ || !cfg_) return;
    const int key = cfg_->sortKey;
    if (key < 0 || key > 3) return;   // 4 = 自定义：保持用户自己拖出来的顺序

    std::vector<DesktopIconView*> mine;
    for (auto* e : *all_)
        if (e && e->owner == index_) mine.push_back(e);
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
        if (e && e->owner == index_) (*all_)[i] = ordered[k++];
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
    y = headerH_ + kPad + r * ch;
    return y + ch <= curH_ - kPad;
}

RECT FenceWindow::rect() const {
    RECT r{};
    if (hwnd_) GetWindowRect(hwnd_, &r);
    return r;
}

RECT FenceWindow::bodyRect() const {
    RECT r = rect();
    r.top += headerH_;
    return r;
}

bool FenceWindow::HitTest(POINT screenPt) const {
    if (!hwnd_) return false;
    RECT r = rect();
    return screenPt.x >= r.left && screenPt.x < r.right &&
           screenPt.y >= r.top && screenPt.y < r.bottom;
}

// ---------------------------------------------------------------------------
// 拖动夹取：先夹进显示器工作区，再沿「穿透最浅」的方向推出其他盒子
// ---------------------------------------------------------------------------
void FenceWindow::ResolveDragPos(RECT want, int& outX, int& outY) const {
    const int w = want.right - want.left;
    const int h = want.bottom - want.top;

    // 1) 屏幕四边（用所在显示器的工作区，避开任务栏）
    POINT probe{want.left, want.top};
    HMONITOR mon = MonitorFromPoint(probe, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi{};
    mi.cbSize = sizeof(mi);
    RECT lim{0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN)};
    if (mon && GetMonitorInfoW(mon, &mi)) lim = mi.rcWork;
    if (lim.right - lim.left < w) lim.right = lim.left + w;
    if (lim.bottom - lim.top < h) lim.bottom = lim.top + h;

    if (want.left < lim.left) want.left = lim.left;
    if (want.top < lim.top) want.top = lim.top;
    if (want.left + w > lim.right) want.left = lim.right - w;
    if (want.top + h > lim.bottom) want.top = lim.bottom - h;

    // 2) 其他盒子：按最小穿透量推开，两遍足够处理夹缝
    if (obstacles_) {
        std::vector<RECT> others = obstacles_();
        for (int pass = 0; pass < 3; ++pass) {
            bool hitAny = false;
            for (const RECT& o : others) {
                RECT r{want.left, want.top, want.left + w, want.top + h};
                if (r.right <= o.left + 1 || o.right <= r.left + 1) continue;
                if (r.bottom <= o.top + 1 || o.bottom <= r.top + 1) continue;
                hitAny = true;

                const int pushLeft = o.left - r.right;      // 负值
                const int pushRight = o.right - r.left;     // 正值
                const int pushUp = o.top - r.bottom;
                const int pushDown = o.bottom - r.top;

                int best = pushLeft;
                if (std::abs(pushRight) < std::abs(best)) best = pushRight;
                if (std::abs(pushUp) < std::abs(best)) best = pushUp;
                if (std::abs(pushDown) < std::abs(best)) best = pushDown;

                if (best == pushLeft || best == pushRight) {
                    want.left += best;
                } else {
                    want.top += best;
                }
            }
            if (!hitAny) break;
            // 推开后可能出界，再夹一次
            if (want.left < lim.left) want.left = lim.left;
            if (want.top < lim.top) want.top = lim.top;
            if (want.left + w > lim.right) want.left = lim.right - w;
            if (want.top + h > lim.bottom) want.top = lim.bottom - h;
        }
    }

    outX = want.left;
    outY = want.top;
}

int FenceWindow::HitItem(POINT clientPt) const {
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
    out.clear();
    if (!all_) return;
    for (auto* e : *all_) {
        if (!e || e->owner != index_) continue;
        // 正在被拖出去的那个仍然占着原来的格子：布局不能动，否则后面的图标
        // 会在松手之前就往前挤。它由 Paint() 画成半透明的「空位」。
        out.push_back(e);
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

void FenceWindow::UpdateDropHint(POINT screenPt) {
    if (!hwnd_) return;
    RECT wr = rect();
    const bool inside = screenPt.x >= wr.left && screenPt.x < wr.right &&
                        screenPt.y >= wr.top && screenPt.y < wr.bottom;

    if (!inside) {
        ClearDropHint();
        return;
    }

    POINT pt{screenPt.x - wr.left, screenPt.y - wr.top};

    // 拖到收起盒子的头部 → 自动展开，才能看清要插到哪
    if (collapsed_ && pt.y < headerH_) {
        autoExpanded_ = true;
        SetCollapsed(false);
        return;
    }
    if (collapsed_) {
        ClearDropHint();
        return;
    }

    DropHint d;
    const int it = (pt.y >= headerH_) ? HitItem(pt) : -1;
    if (it >= 0) {
        RECT ir{};
        ItemRect(it, ir);
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
        if (!resizing_ && pressItem_ >= 0) {
            const int item = pressItem_;
            sel_ = item;
            pressItem_ = -1;    // 随后的 LBUTTONUP 别再打开一次
            pressBody_ = false;
            std::vector<DesktopIconView*> mine;
            Collect(mine);
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
                if (!dragging_ && (std::abs(dx) > 2 || std::abs(dy) > 2)) dragging_ = true;
                if (dragging_) {
                    RECT want = dragStartRect_;
                    want.left += dx;
                    want.top += dy;
                    want.right = want.left + (dragStartRect_.right - dragStartRect_.left);
                    want.bottom = want.top + (dragStartRect_.bottom - dragStartRect_.top);

                    int nx = 0, ny = 0;
                    ResolveDragPos(want, nx, ny);
                    if (nx != dragLastX_ || ny != dragLastY_) {
                        dragLastX_ = nx;
                        dragLastY_ = ny;
                        SetWindowPos(h, nullptr, nx, ny, 0, 0,
                                     SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
                    }
                    return 0;
                }
            }

            if (pressBody_ && pressItem_ >= 0) {
                if (!itemDragging_ && (std::abs(dx) > 4 || std::abs(dy) > 4)) {
                    itemDragging_ = true;
                    std::vector<DesktopIconView*> mine;
                    Collect(mine);
                    if (pressItem_ < (int)mine.size()) {
                        dragged_ = mine[pressItem_];
                        if (onItemDragStart) onItemDragStart(dragged_, cur);
                    }
                    Redraw();
                }
                if (itemDragging_) {
                    if (dragged_ && onItemDragMove) onItemDragMove(dragged_, cur);
                    return 0;
                }
            }
        }

        // 悬停自动展开：收起状态下鼠标移到头部就展开（移开整个盒子在 MOUSELEAVE 里收起）
        if (g_config.autoExpandOnHover && collapsed_ && !suppressAutoExpand_ &&
            pt.y < headerH_) {
            autoExpanded_ = true;
            SetCollapsed(false);
        }

        int hot = -1;
        if (!collapsed_ && pt.y >= headerH_) hot = HitItem(pt);
        if (hot != hot_) {
            hot_ = hot;
            Redraw();
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
        if (autoExpanded_) {
            if (itemDragging_ || dragging_) {
                autoExpanded_ = false;   // 正在操作，别收
            } else {
                autoExpanded_ = false;
                SetCollapsed(true);
            }
        }
        return 0;

    case WM_RBUTTONUP: {
        // 右键菜单：盒子内是自绘菜单（不是系统菜单）
        POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        POINT sp = pt;
        ClientToScreen(h, &sp);
        const int it = (pt.y >= headerH_ && !collapsed_) ? HitItem(pt) : -1;
        if (onContextMenu) onContextMenu(it, sp);
        return 0;
    }

    case WM_LBUTTONUP: {
        POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        POINT cur{};
        GetCursorPos(&cur);
        if (GetCapture() == h) ReleaseCapture();

        const int item = pressItem_;
        const bool arrow = pressArrow_;
        const bool wasDragging = dragging_;
        const bool wasItemDrag = itemDragging_;
        const bool wasResizing = resizing_;

        dragging_ = false;
        itemDragging_ = false;
        resizing_ = false;
        resizeEdge_ = Edge::None;
        pressItem_ = -1;
        pressArrow_ = false;
        pressHeader_ = false;
        pressBody_ = false;

        if (wasResizing) {
            if (onGeometryChanged) onGeometryChanged();
            Redraw();
            return 0;
        }
        if (wasDragging) {
            if (onGeometryChanged) onGeometryChanged();
            Redraw();
            return 0;
        }
        if (wasItemDrag) {
            // 用拖动开始时缓存的指针：松手后 owner 可能已经变了
            // 注意：这里临时把 itemDragging_ 恢复成 true，让 CollectVisible 仍然排除被拖的图标，
            // 这样 main 拿到的 dropHint 下标和拖动过程中显示的一致。
            itemDragging_ = true;
            if (dragged_ && onItemDrop) onItemDrop(dragged_, cur);
            itemDragging_ = false;
            dragged_ = nullptr;
            Redraw();
            return 0;
        }
        if (arrow) {
            autoExpanded_ = false;   // 手动操作以用户意图为准
            SetCollapsed(!collapsed_);
            suppressAutoExpand_ = collapsed_;   // 刚收起且光标还在头上，别又弹开
            return 0;
        }
        if (item >= 0) {
            // 单击只选中，双击（WM_LBUTTONDBLCLK）才打开
            sel_ = item;
            Redraw();
        } else if (pt.y >= headerH_ && sel_ != -1) {
            sel_ = -1;   // 点空白处取消选中
            Redraw();
        }
        (void)pt;
        return 0;
    }

    case WM_TIMER: {
        if (wp != kAnimTimer) break;
        bool busy = false;

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

        // 排序 flip 动画
        if (flipActive_) {
            if (GetTickCount() - flipT0_ >= (DWORD)flipDur_) {
                StopFlip();     // 收尾时还要再画一帧，让图标回到最终位置
            }
            busy = true;
        }

        if (!busy) {
            KillTimer(h, kAnimTimer);
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

void FenceWindow::RefreshBackdrop() {
    backdrop_.clear();
    backdropW_ = backdropH_ = 0;
    if (!cfg_ || !cfg_->acrylic || !hwnd_) return;
    RECT r = rect();
    if (r.right <= r.left || r.bottom <= r.top) return;

    // 一律按「展开尺寸」抓：收起时只用到上面一段（行距仍是 width_，可直接裁）
    const int w = width_;
    const int h = height_ > 0 ? height_ : (r.bottom - r.top);

    // 抓屏兜底路径要把自己排除掉，避免"抓到上一帧的自己"形成回授
    SetWindowDisplayAffinity(hwnd_, WDA_EXCLUDEFROMCAPTURE);
    wchar_t mode[32]{};
    GetEnvironmentVariableW(L"MEOW_BACKDROP", mode, 32);
    const bool preferDesktop = _wcsicmp(mode, L"desktop") == 0;
    const bool ok = CaptureBlurredBackdrop(r.left, r.top, w, h, 20, backdrop_, preferDesktop);
    SetWindowDisplayAffinity(hwnd_, WDA_NONE);
    if (!ok) {
        backdrop_.clear();
        return;
    }
    backdropW_ = w;
    backdropH_ = h;
    backdropX_ = r.left;
    backdropY_ = r.top;
}

void FenceWindow::Paint() {
    if (!bits_ || !memDC_ || !cfg_) return;
    std::memset(bits_, 0, (size_t)bufW_ * (size_t)bufH_ * 4);

    const COLORREF base = (COLORREF)cfg_->color;
    const int alpha = std::clamp(cfg_->alpha, 0, 255);
    const double rad = (double)std::max(0, cfg_->radius);
    const bool light = IsLight(base);

    const BYTE r = GetRValue(base), g = GetGValue(base), b = GetBValue(base);
    const bool hasBackdrop =
        cfg_->acrylic && backdropW_ == bufW_ && backdropH_ >= bufH_ &&
        backdrop_.size() >= (size_t)bufW_ * (size_t)bufH_ * 4;

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
            // 亚克力：模糊壁纸打底 + 半透明着色，整体不透明
            Bitmap blur(bufW_, bufH_, bufW_ * 4, PixelFormat32bppPARGB, backdrop_.data());
            gfx.SetClip(&shape);
            gfx.DrawImage(&blur, Rect(0, 0, bufW_, bufH_));
            gfx.ResetClip();
            SolidBrush tint{Color((BYTE)std::min(255, (int)(alpha * 0.58)), r, g, b)};
            gfx.FillPath(&tint, &shape);
        } else {
            SolidBrush body{Color((BYTE)alpha, r, g, b)};
            gfx.FillPath(&body, &shape);
        }

        // ---- 头部：略亮、略实，做出层叠感 ----
        const float headH = (float)headerH_;
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
        SolidBrush headBrush{Color(hA, (BYTE)Lighten(r, 0.45), (BYTE)Lighten(g, 0.45),
                                    (BYTE)Lighten(b, 0.45))};
        gfx.FillPath(&headBrush, &head);

        // ---- 描边 / 分隔线 ----
        Pen border{Color(46, 0, 0, 0), 1.0f};
        gfx.DrawPath(&border, &shape);
        Pen sep{Color(30, 0, 0, 0), 1.0f};
        gfx.DrawLine(&sep, 0.0f, headH, (float)bufW_, headH);

        const ARGB fg = light ? 0xFF1B1B1F : 0xFFF2F2F5;

        // ---- 标题（受「显示标题」开关控制）----
        if (g_config.showLabels) {
            FontFamily family(L"Microsoft YaHei UI");
            const float fs = headerH_ >= 40 ? 14.0f : 12.5f;
            Font titleFont(&family, fs, FontStyleRegular, UnitPixel);
            SolidBrush brush{Color(fg)};
            StringFormat fmt;
            fmt.SetLineAlignment(StringAlignmentCenter);
            fmt.SetFormatFlags(StringFormatFlagsNoWrap | StringFormatFlagsNoClip);
            fmt.SetTrimming(StringTrimmingEllipsisCharacter);
            gfx.DrawString(cfg_->title.c_str(), -1, &titleFont,
                           RectF(16.0f, 0.0f, (float)bufW_ - 56.0f, headH),
                           &fmt, &brush);
        }

        // ---- 收起 / 展开箭头 ----
        {
            Pen pen{Color(fg), 1.8f};
            pen.SetStartCap(LineCapRound);
            pen.SetEndCap(LineCapRound);
            const float cx = (float)bufW_ - 21.0f;
            const float cy = headH * 0.5f;
            if (collapsed_) {
                gfx.DrawLine(&pen, cx - 2.5f, cy - 5.0f, cx + 2.5f, cy);
                gfx.DrawLine(&pen, cx + 2.5f, cy, cx - 2.5f, cy + 5.0f);
            } else {
                gfx.DrawLine(&pen, cx - 5.0f, cy - 2.5f, cx, cy + 2.5f);
                gfx.DrawLine(&pen, cx, cy + 2.5f, cx + 5.0f, cy - 2.5f);
            }
        }

        // ---- 悬停高亮（拖动中不画：被拖的那个已变成"空位"，再套一层高亮很乱）----
        if (!collapsed_ && hot_ >= 0 && !itemDragging_) {
            RECT ir{};
            ItemRect(hot_, ir);
            if (ir.right > ir.left) {
                GraphicsPath cell;
                AddRoundRect(cell, RectF((float)ir.left + 2, (float)ir.top + 2,
                                         (float)(ir.right - ir.left) - 4,
                                         (float)cellH() - 4),
                             8.0f);
                SolidBrush hl{light ? Color(26, 0, 0, 0) : Color(52, 255, 255, 255)};
                gfx.FillPath(&hl, &cell);
            }
        }

        // ---- 单击选中高亮（双击才打开，所以要有可见的选中态）----
        if (!collapsed_ && sel_ >= 0 && !itemDragging_) {
            RECT sr{};
            ItemRect(sel_, sr);
            if (sr.right > sr.left) {
                GraphicsPath cell;
                AddRoundRect(cell,
                             RectF((float)sr.left + 3.0f, (float)sr.top + 3.0f,
                                   (float)(sr.right - sr.left) - 6.0f,
                                   (float)cellH() - 6.0f),
                             8.0f);
                // 浅色卡片上用暗一点的高亮，深色卡片上用亮一点的
                SolidBrush fill{light ? Color(34, 0, 0, 0) : Color(44, 255, 255, 255)};
                gfx.FillPath(&fill, &cell);
                Pen pen{light ? Color(70, 0, 0, 0) : Color(80, 255, 255, 255), 1.0f};
                gfx.DrawPath(&pen, &cell);
            }
        }

        // ---- 拖拽落点提示 ----
        if (!collapsed_ && drop_.item >= 0) {
            RECT ir{};
            ItemRect(drop_.item, ir);
            if (ir.right > ir.left) {
                const Color accentC(255, 0, 120, 215);
                if (drop_.zone == 0) {
                    // 中间：高亮整个格子 → 松手 = 用该图标对应的程序打开拖拽文件
                    GraphicsPath cell;
                    AddRoundRect(cell,
                                 RectF((float)ir.left + 3.0f, (float)ir.top + 3.0f,
                                       (float)(ir.right - ir.left) - 6.0f,
                                       (float)cellH() - 6.0f),
                                 8.0f);
                    SolidBrush fill{Color(58, 0, 120, 215)};
                    gfx.FillPath(&fill, &cell);
                    Pen pen{accentC, 1.6f};
                    gfx.DrawPath(&pen, &cell);
                } else {
                    // 左/右：画一条竖缝隙线，告诉用户松手会插在哪
                    const float lx = (drop_.zone < 0) ? (float)ir.left + 0.5f
                                                      : (float)ir.right - 3.5f;
                    GraphicsPath bar;
                    AddRoundRect(bar,
                                 RectF(lx, (float)ir.top + 6.0f, 3.0f,
                                       (float)cellH() - 12.0f),
                                 1.5f);
                    SolidBrush ab{accentC};
                    gfx.FillPath(&ab, &bar);
                }
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

    // 本帧每个图标落在哪个槽位 / 横向缩放到多少（排序 flip 动画；平时 slot==i、scale==1）
    std::vector<DesktopIconView*> mine;
    CollectVisible(mine);
    std::vector<int> slotOf(mine.size());
    std::vector<float> scaleOf(mine.size(), 1.0f);
    {
        const float fp = FlipProgress();
        for (size_t i = 0; i < mine.size(); ++i) {
            slotOf[i] = (int)i;
            if (!flipActive_) continue;
            auto f = flipNodes_.find(mine[i]);
            if (f == flipNodes_.end()) continue;
            const FlipNode& n = f->second;
            if (fp < 0.5f) {
                slotOf[i] = n.from;
                scaleOf[i] = std::cos(fp / 0.5f * 1.5707963f);          // 1 → 0
            } else {
                slotOf[i] = n.to;
                scaleOf[i] = std::sin((fp - 0.5f) / 0.5f * 1.5707963f);  // 0 → 1
            }
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
            int ix = 0, iy = 0;
            if (!ItemOrigin(slotOf[i], ix, iy)) continue;
            const float scaleX = scaleOf[i];

            // 正在被拖的那个留在原位当「空位」：变淡 + 变暗，松手后才重新排位
            const bool ghost = itemDragging_ && e == dragged_;
            const int ga = ghost ? 110 : 255;   // 图标不透明度
            const int gd = ghost ? 120 : 255;   // 额外压暗
            // flip 时只有文字按翻转程度淡出/淡入；图标本身保持不透明，
            // 靠横向压扁到 1px 来表现「翻过去」——两者一起淡会变成交叉淡化，不像翻转。
            int gaLabel = ga;
            if (!ghost && scaleX < 0.999f)
                gaLabel = (int)std::lround(255.0f * scaleX);

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

            // flip 时绕图标中线横向压扁/张开
            const bool squashed = scaleX < 0.999f;
            int outW = iconW;
            int drawX = iconX;
            if (squashed) {
                outW = std::max(1, (int)std::lround(iconW * scaleX));
                drawX = iconX + (iconW - outW) / 2;
            }
            if (bm.valid()) {
                if (squashed) {
                    BlendIntoDimScaledX(dst, bufW_, bufH_, bm.pixels.data(), bm.width,
                                        bm.height, drawX, iconY, outW, ga, gd);
                } else {
                    BlendIntoDim(dst, bufW_, bufH_, bm.pixels.data(), bm.width, bm.height,
                                 drawX, iconY, ga, gd);
                }
            }

            if (e->name.empty()) continue;

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
                if (listMode) {
                    const int labW = std::max(24, (width_ - kPad * 2) - (iconX - ix) - iconW - 8);
                    LabelDib* lab = LabelOf(e, labW, iconH, glow);
                    if (lab && lab->valid()) {
                        const int left = iconX + iconW + 8 - lab->pad;
                        const int top = iy + (ch - lab->h) / 2;
                        BlendIntoDim(dst, bufW_, bufH_,
                                     reinterpret_cast<const BYTE*>(lab->bits), lab->w, lab->h,
                                     left, top, gaLabel, gd);
                    }
                } else {
                    const int iconBottom = iy + 4 + iconH;
                    const int textTop = iconBottom + 2;
                    int th = ch - (textTop - iy);
                    if (th < 14) th = 14;
                    int labLeft = 0, labW = 0;
                    ItemLabelBox(slotOf[i], labLeft, labW);
                    LabelDib* lab = LabelOf(e, labW, th, glow);
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
                int ix = 0, iy = 0;
                if (!ItemOrigin(slotOf[i], ix, iy)) continue;
                auto d = detailInfo_.find(mine[i]);
                if (d == detailInfo_.end()) continue;

                const DetailCols dl = DetailLayoutFor(kPad, width_ - kPad * 2);
                int ga = 255;
                const bool ghost = itemDragging_ && mine[i] == dragged_;
                if (ghost) ga = 110;
                if (!ghost && scaleOf[i] < 0.999f) {
                    const int fa = (int)std::lround(255.0f * scaleOf[i]);
                    if (fa < ga) ga = fa;
                }
                if (ga <= 0) continue;

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
}

LabelDib* FenceWindow::LabelOf(DesktopIconView* e, int boxW, int boxH, int glow) {
    std::wstring key = e->name + L"\x1|" + std::to_wstring(boxW) + L"x" +
                       std::to_wstring(boxH) + L"g" + std::to_wstring(glow);
    auto k = labelKeys_.find(e);
    if (k != labelKeys_.end() && k->second == key) {
        auto it = labels_.find(e);
        if (it != labels_.end()) return &it->second;
    }
    LabelDib d;
    // 与桌面同一套参数：白色 + 光晕，盒子内只允许一行
    if (!d.Build(e->name, IconLabelFont(), boxW, boxH, glow, RGB(255, 255, 255), 1)) {
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
    EnsureBackbuffer(width_, curH_);
    Paint();
    Present();
}

void FenceWindow::ApplyConfig(bool recapture) {
    if (!hwnd_ || !cfg_) return;

    const int newHeader = std::clamp(g_config.headerHeight, 26, 80);
    const bool newLabels = g_config.showLabels != 0;
    if (newHeader != headerH_ || newLabels != showLabels_) {
        headerH_ = newHeader;
        showLabels_ = newLabels;
        labels_.clear();
        labelKeys_.clear();
        // 收起高度跟着头部走
        if (collapsed_) {
            targetH_ = headerH_;
            curH_ = headerH_;
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
    headerH_ = std::clamp(g_config.headerHeight, 26, 80);
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
    targetH_ = collapsed ? headerH_ : height_;
    if (hwnd_) SetTimer(hwnd_, kAnimTimer, 16, nullptr);
    Redraw();
}

void FenceWindow::Destroy() {
    if (hwnd_) {
        KillTimer(hwnd_, kAnimTimer);
        SetWindowLongPtrW(hwnd_, GWLP_USERDATA, 0);
        DestroyWindow(hwnd_);
        hwnd_ = nullptr;
    }
    if (dib_) { DeleteObject(dib_); dib_ = nullptr; }
    if (memDC_) { DeleteDC(memDC_); memDC_ = nullptr; }
    bits_ = nullptr;
    bufW_ = bufH_ = 0;
    backdrop_.clear();
    labels_.clear();
    labelKeys_.clear();
    detailInfo_.clear();
    detailText_.clear();
    StopFlip();
}

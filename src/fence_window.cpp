#include "fence_window.h"

#include <windowsx.h>
#include <objidl.h>
#include <propidl.h>
#include <gdiplus.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <memory>
#include <vector>

#include "backdrop.h"
#include "icon_render.h"

#pragma comment(lib, "gdiplus.lib")

using namespace Gdiplus;

namespace {

const wchar_t* kClass = L"MeowDeskFence";

const int kPad = 14;
const int kCellW = 74;
const int kCellH = 62;

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

bool IsLight(COLORREF c) {
    double l = 0.299 * GetRValue(c) + 0.587 * GetGValue(c) + 0.114 * GetBValue(c);
    return l > 140.0;
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
    int n = (width_ - kPad * 2) / kCellW;
    if (n < 1) n = 1;
    return n;
}

int FenceWindow::minW() const {
    return kPad * 2 + kCellW;   // 至少放得下一列
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
    // 尺寸变了，图标可能换行/换列
    sel_ = -1;
    hot_ = -1;
    Redraw();
}

bool FenceWindow::ItemOrigin(int index, int& x, int& y) const {
    const int cols = CellCols();
    const int gridW = cols * kCellW;
    int ox = (width_ - gridW) / 2;
    if (ox < kPad) ox = kPad;
    if (index < 0) return false;
    const int c = index % cols;
    const int r = index / cols;
    const int ch = cellH();
    x = ox + c * kCellW;
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
    const int ch = cellH();
    for (size_t i = 0; i < mine.size(); ++i) {
        int ix = 0, iy = 0;
        if (!ItemOrigin((int)i, ix, iy)) continue;
        if (clientPt.x >= ix && clientPt.x < ix + kCellW &&
            clientPt.y >= iy && clientPt.y < iy + ch)
            return (int)i;
    }
    return -1;
}

void FenceWindow::CollectVisible(std::vector<DesktopIconView*>& out) const {
    out.clear();
    if (!all_) return;
    for (auto* e : *all_) {
        if (!e || e->owner != index_) continue;
        // 正在被拖出去的那个不参与布局（视觉上它已经跟着光标走了）
        if (itemDragging_ && e == dragged_) continue;
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
        int ix = 0, iy = 0;
        ItemOrigin(it, ix, iy);
        const int third = kCellW / 3;
        const int rx = pt.x - ix;
        d.item = it;
        if (rx < third) {
            d.zone = -1;
            d.insert = it;
        } else if (rx >= kCellW - third) {
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
        const int diff = targetH_ - curH_;
        if (diff == 0) {
            KillTimer(h, kAnimTimer);
            return 0;
        }
        int step = (int)std::lround(diff * 0.35);
        if (step == 0) step = diff > 0 ? 4 : -4;
        curH_ += step;
        if ((diff > 0 && curH_ > targetH_) || (diff < 0 && curH_ < targetH_)) curH_ = targetH_;
        Redraw();
        return 0;
    }

    case WM_CLOSE:
        // 盒子不负责退出程序（可能是被外部关掉的），交给 main 处理
        return 0;

    case WM_DESTROY:
        return 0;
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

        // ---- 悬停高亮 ----
        if (!collapsed_ && hot_ >= 0) {
            int ix = 0, iy = 0;
            if (ItemOrigin(hot_, ix, iy)) {
                GraphicsPath cell;
                AddRoundRect(cell, RectF((float)ix + 2, (float)iy + 2,
                                         (float)kCellW - 4, (float)cellH() - 4),
                             8.0f);
                SolidBrush hl{light ? Color(26, 0, 0, 0) : Color(52, 255, 255, 255)};
                gfx.FillPath(&hl, &cell);
            }
        }

        // ---- 单击选中高亮（双击才打开，所以要有可见的选中态）----
        if (!collapsed_ && sel_ >= 0) {
            int sx = 0, sy = 0;
            if (ItemOrigin(sel_, sx, sy)) {
                GraphicsPath cell;
                AddRoundRect(cell,
                             RectF((float)sx + 3.0f, (float)sy + 3.0f, (float)kCellW - 6.0f,
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
            int ix = 0, iy = 0;
            if (ItemOrigin(drop_.item, ix, iy)) {
                const Color accentC(255, 0, 120, 215);
                if (drop_.zone == 0) {
                    // 中间：高亮整个格子 → 松手 = 用该图标对应的程序打开拖拽文件
                    GraphicsPath cell;
                    AddRoundRect(cell,
                                 RectF((float)ix + 3.0f, (float)iy + 3.0f,
                                       (float)kCellW - 6.0f, (float)cellH() - 6.0f),
                                 8.0f);
                    SolidBrush fill{Color(58, 0, 120, 215)};
                    gfx.FillPath(&fill, &cell);
                    Pen pen{accentC, 1.6f};
                    gfx.DrawPath(&pen, &cell);
                } else {
                    // 左/右：画一条竖缝隙线，告诉用户松手会插在哪
                    const float lx = (drop_.zone < 0) ? (float)ix + 0.5f
                                                      : (float)(ix + kCellW) - 3.5f;
                    GraphicsPath bar;
                    AddRoundRect(bar,
                                 RectF(lx, (float)iy + 6.0f, 3.0f, (float)cellH() - 12.0f),
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

    // ---- 图标（手工预乘合成，1:1 无重采样）+ 标题 ----
    if (!collapsed_) {
        std::vector<DesktopIconView*> mine;
        CollectVisible(mine);
        auto* dst = reinterpret_cast<BYTE*>(bits_);
        const int ch = cellH();
        for (size_t i = 0; i < mine.size(); ++i) {
            int ix = 0, iy = 0;
            if (!ItemOrigin((int)i, ix, iy)) break;

            const IconBitmap& bm = mine[i]->bmp;
            if (bm.valid()) {
                int iyIcon = iy + (showLabels_ ? 4 : (ch - bm.height) / 2);
                BlendInto(dst, bufW_, bufH_, bm.pixels.data(), bm.width, bm.height,
                          ix + (kCellW - bm.width) / 2, iyIcon);
            }

            if (showLabels_ && !mine[i]->name.empty()) {
                const int iconBottom = iy + 4 + (bm.valid() ? bm.height : 48);
                const int textTop = iconBottom + 2;
                int textH = ch - (textTop - iy);
                if (textH < 14) textH = 14;
                // 和桌面图标标题保持一致：白字 + 柔和黑光晕，光晕受「文字阴影」开关控制
                const int glow = g_config.textShadow ? 2 : 0;
                LabelDib* lab = LabelOf(mine[i], kCellW - 8, textH, glow);
                if (lab && lab->valid()) {
                    int contentW = lab->w - lab->pad * 2;
                    int left = ix + (kCellW - contentW) / 2 - lab->pad;
                    BlendInto(dst, bufW_, bufH_,
                              reinterpret_cast<const BYTE*>(lab->bits), lab->w, lab->h,
                              left, textTop - lab->pad);
                }
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
    curH_ = h;
    targetH_ = h;

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
}

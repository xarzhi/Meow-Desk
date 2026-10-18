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

const int kHeaderH = 44;
const int kPad = 14;
const int kCols = 4;
const int kCellW = 74;
const int kCellH = 62;

const UINT_PTR kAnimTimer = 2;

void AddRoundRect(GraphicsPath& p, const RectF& r, float rad) {
    float d = rad * 2.0f;
    if (d > r.Width) d = r.Width;
    if (d > r.Height) d = r.Height;
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

bool FenceWindow::ItemOrigin(int index, int& x, int& y) const {
    const int gridW = kCols * kCellW;
    int ox = (width_ - gridW) / 2;
    if (ox < kPad) ox = kPad;
    if (index < 0) return false;
    const int c = index % kCols;
    const int r = index / kCols;
    x = ox + c * kCellW;
    y = kHeaderH + kPad + r * kCellH;
    return y + kCellH <= curH_ - kPad;
}

RECT FenceWindow::rect() const {
    RECT r{};
    if (hwnd_) GetWindowRect(hwnd_, &r);
    return r;
}

RECT FenceWindow::bodyRect() const {
    RECT r = rect();
    r.top += kHeaderH;
    return r;
}

bool FenceWindow::HitTest(POINT screenPt) const {
    if (!hwnd_) return false;
    RECT r = rect();
    return screenPt.x >= r.left && screenPt.x < r.right &&
           screenPt.y >= r.top && screenPt.y < r.bottom;
}

int FenceWindow::HitItem(POINT clientPt) const {
    std::vector<DesktopIconView*> mine;
    Collect(mine);
    for (size_t i = 0; i < mine.size(); ++i) {
        int ix = 0, iy = 0;
        if (!ItemOrigin((int)i, ix, iy)) continue;
        if (clientPt.x >= ix && clientPt.x < ix + kCellW &&
            clientPt.y >= iy && clientPt.y < iy + kCellH)
            return (int)i;
    }
    return -1;
}

LRESULT FenceWindow::WndProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_MOUSEACTIVATE:
        return MA_NOACTIVATE;

    case WM_LBUTTONDOWN: {
        POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        pressPt_ = pt;
        // 拖动必须用屏幕坐标：窗口一动客户区坐标基准就跟着动
        GetCursorPos(&pressScreenPt_);
        GetWindowRect(h, &dragStartRect_);
        dragLastX_ = dragStartRect_.left;
        dragLastY_ = dragStartRect_.top;
        dragging_ = false;
        itemDragging_ = false;
        pressItem_ = -1;
        pressArrow_ = false;
        pressHeader_ = false;
        pressBody_ = false;

        if (pt.y < kHeaderH && pt.x > width_ - 40) {
            pressArrow_ = true;
        } else if (pt.y < kHeaderH) {
            pressHeader_ = true;
        } else if (!collapsed_) {
            pressBody_ = true;
            pressItem_ = HitItem(pt);
        }
        SetCapture(h);
        Redraw();
        return 0;
    }

    case WM_MOUSEMOVE: {
        POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        POINT cur{};
        GetCursorPos(&cur);

        if (GetCapture() == h) {
            const int dx = cur.x - pressScreenPt_.x;
            const int dy = cur.y - pressScreenPt_.y;

            if ((pressHeader_ || pressArrow_) && !itemDragging_) {
                if (!dragging_ && (std::abs(dx) > 2 || std::abs(dy) > 2)) dragging_ = true;
                if (dragging_) {
                    const int nx = dragStartRect_.left + dx;
                    const int ny = dragStartRect_.top + dy;
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

        int hot = -1;
        if (!collapsed_ && pt.y >= kHeaderH) hot = HitItem(pt);
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
        if (hot_ != -1) { hot_ = -1; Redraw(); }
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

        dragging_ = false;
        itemDragging_ = false;
        pressItem_ = -1;
        pressArrow_ = false;
        pressHeader_ = false;
        pressBody_ = false;

        if (wasDragging) {
            if (onGeometryChanged) onGeometryChanged();
            Redraw();
            return 0;
        }
        if (wasItemDrag) {
            // 用拖动开始时缓存的指针：松手后 owner 可能已经变了
            if (dragged_ && onItemDrop) onItemDrop(dragged_, cur);
            dragged_ = nullptr;
            Redraw();
            return 0;
        }
        if (arrow) {
            SetCollapsed(!collapsed_);
            return 0;
        }
        if (item >= 0) {
            std::vector<DesktopIconView*> mine;
            Collect(mine);
            if (item < (int)mine.size() && onOpen) onOpen(mine[item]);
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
    if (!cfg_ || !cfg_->acrylic || !hwnd_) return;
    RECT r = rect();
    if (r.right <= r.left || r.bottom <= r.top) return;

    // 把自己排除在截屏之外，避免"抓到上一帧的自己"形成回授
    SetWindowDisplayAffinity(hwnd_, WDA_EXCLUDEFROMCAPTURE);
    CaptureBlurredBackdrop(r.left, r.top, r.right - r.left, r.bottom - r.top, 20, backdrop_);
    SetWindowDisplayAffinity(hwnd_, WDA_NONE);

    backdropX_ = r.left;
    backdropY_ = r.top;
    if (backdrop_.size() != (size_t)bufW_ * bufH_ * 4 &&
        backdrop_.size() != (size_t)(r.right - r.left) * (r.bottom - r.top) * 4) {
        backdrop_.clear();
    }
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
        cfg_->acrylic && backdrop_.size() == (size_t)bufW_ * bufH_ * 4;

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
            SolidBrush tint{Color((BYTE)std::min(255, (int)(alpha * 0.62)), r, g, b)};
            gfx.FillPath(&tint, &shape);
        } else {
            SolidBrush body{Color((BYTE)alpha, r, g, b)};
            gfx.FillPath(&body, &shape);
        }

        // ---- 头部：略亮、略实，做出层叠感 ----
        GraphicsPath head;
        {
            const float d = (float)rad * 2.0f;
            head.AddArc(0.0f, 0.0f, d, d, 180.0f, 90.0f);
            head.AddArc((float)bufW_ - d, 0.0f, d, d, 270.0f, 90.0f);
            head.AddLine((float)bufW_, (float)kHeaderH, 0.0f, (float)kHeaderH);
            head.CloseFigure();
        }
        const BYTE hA = hasBackdrop ? (BYTE)std::min(255, (int)(alpha * 0.80))
                                    : (BYTE)std::min(255, alpha + 42);
        SolidBrush headBrush{Color(hA, (BYTE)Lighten(r, 0.45), (BYTE)Lighten(g, 0.45),
                                    (BYTE)Lighten(b, 0.45))};
        gfx.FillPath(&headBrush, &head);

        // ---- 描边 / 分隔线 ----
        Pen border{Color(46, 0, 0, 0), 1.0f};
        gfx.DrawPath(&border, &shape);
        Pen sep{Color(30, 0, 0, 0), 1.0f};
        gfx.DrawLine(&sep, 0.0f, (float)kHeaderH, (float)bufW_, (float)kHeaderH);

        const ARGB fg = light ? 0xFF1B1B1F : 0xFFF2F2F5;

        // ---- 标题 ----
        {
            FontFamily family(L"Microsoft YaHei UI");
            Font titleFont(&family, 14.0f, FontStyleRegular, UnitPixel);
            SolidBrush brush{Color(fg)};
            StringFormat fmt;
            fmt.SetLineAlignment(StringAlignmentCenter);
            fmt.SetFormatFlags(StringFormatFlagsNoWrap | StringFormatFlagsNoClip);
            fmt.SetTrimming(StringTrimmingEllipsisCharacter);
            gfx.DrawString(cfg_->title.c_str(), -1, &titleFont,
                           RectF(16.0f, 0.0f, (float)bufW_ - 56.0f, (float)kHeaderH),
                           &fmt, &brush);
        }

        // ---- 收起 / 展开箭头 ----
        {
            Pen pen{Color(fg), 1.8f};
            pen.SetStartCap(LineCapRound);
            pen.SetEndCap(LineCapRound);
            const float cx = (float)bufW_ - 21.0f;
            const float cy = (float)kHeaderH * 0.5f;
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
                                         (float)kCellW - 4, (float)kCellH - 4),
                             8.0f);
                SolidBrush hl{Color(52, 255, 255, 255)};
                gfx.FillPath(&hl, &cell);
            }
        }

        // ---- 空状态提示 ----
        std::vector<DesktopIconView*> mineEmpty;
        Collect(mineEmpty);
        if (!collapsed_ && mineEmpty.empty()) {
            FontFamily family(L"Microsoft YaHei UI");
            Font hintFont(&family, 12.0f, FontStyleRegular, UnitPixel);
            SolidBrush h2{Color((BYTE)(light ? 150 : 170), (BYTE)(light ? 90 : 210),
                                 (BYTE)(light ? 96 : 210), (BYTE)(light ? 108 : 210))};
            StringFormat hf;
            hf.SetAlignment(StringAlignmentCenter);
            hf.SetLineAlignment(StringAlignmentCenter);
            gfx.DrawString(L"\u628a\u684c\u9762\u56fe\u6807\u62d6\u8fdb\u6765", -1, &hintFont,
                           RectF(0.0f, (float)kHeaderH, (float)bufW_,
                                 (float)bufH_ - (float)kHeaderH),
                           &hf, &h2);
        }

        gfx.Flush(FlushIntentionSync);
    }

    // ---- 图标（手工预乘合成，1:1 无重采样）----
    if (!collapsed_) {
        std::vector<DesktopIconView*> mine;
        Collect(mine);
        auto* dst = reinterpret_cast<BYTE*>(bits_);
        for (size_t i = 0; i < mine.size(); ++i) {
            if (itemDragging_ && (int)i == pressItem_) continue;  // 拖出去的那个不画
            int ix = 0, iy = 0;
            if (!ItemOrigin((int)i, ix, iy)) break;
            const IconBitmap& bm = mine[i]->bmp;
            if (!bm.valid()) continue;
            BlendInto(dst, bufW_, bufH_, bm.pixels.data(), bm.width, bm.height,
                      ix + (kCellW - bm.width) / 2, iy + (kCellH - bm.height) / 2);
        }
    }
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
    wc.lpfnWndProc = WndProcStatic;
    wc.hInstance = inst;
    wc.lpszClassName = kClass;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    RegisterClassExW(&wc);

    width_ = w;
    height_ = h;
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
    targetH_ = collapsed ? kHeaderH : height_;
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
}

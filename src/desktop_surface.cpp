#include "desktop_surface.h"

#include "icon_render.h"
#include "shell_icons.h"

#include <objidl.h>
#include <ole2.h>
#include <windowsx.h>

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace {

const wchar_t* kClassName = L"MeowDeskSurface";

const int kIconTopPad = DesktopSurface::kIconTopPad;
const int kTextGap = DesktopSurface::kTextGap;
const int kLabelInset = 10;    // 标签左右内缩（对齐资源管理器的折行宽度）

inline int SnapTo(int v, int step) {
    if (step <= 0) return v;
    return (int)std::lround((double)v / (double)step) * step;
}

RECT Normalize(POINT a, POINT b) {
    RECT r{};
    r.left = std::min(a.x, b.x);
    r.top = std::min(a.y, b.y);
    r.right = std::max(a.x, b.x);
    r.bottom = std::max(a.y, b.y);
    return r;
}

bool Intersects(const RECT& a, const RECT& b) {
    return a.left < b.right && b.left < a.right && a.top < b.bottom && b.top < a.bottom;
}

void LogSurface(const char* fmt, ...) {
    FILE* f = nullptr;
    fopen_s(&f, "meow-desk.log", "a");
    if (!f) return;
    va_list ap;
    va_start(ap, fmt);
    vfprintf(f, fmt, ap);
    va_end(ap);
    fputc('\n', f);
    fclose(f);
}

}  // namespace

struct DesktopSurface::Impl {
    std::unordered_map<DesktopIconView*, IconDib> icons;
    std::unordered_map<DesktopIconView*, LabelDib> labels;
    std::unordered_map<DesktopIconView*, std::wstring> labelKeys;
    // 诊断用：跳过 DIB 缓存，用来分离「图标 DIB / 文字 DIB」各自占的内存
    bool noCache = GetEnvironmentVariableW(L"MEOW_PROBE_NOCACHE", nullptr, 0) > 0;

    IconDib* Icon(DesktopIconView* e) {
        if (noCache) return nullptr;
        auto it = icons.find(e);
        if (it != icons.end()) return &it->second;
        IconDib d;
        if (!d.Build(e->bmp)) {
            icons.emplace(e, IconDib{});
            return nullptr;
        }
        auto r = icons.emplace(e, std::move(d));
        return &r.first->second;
    }

    LabelDib* Label(DesktopIconView* e, int boxW, int boxH, int glow) {
        if (noCache) return nullptr;
        std::wstring key = e->name + L"\x1|" + std::to_wstring(boxW) + L"x" +
                           std::to_wstring(boxH) + L"g" + std::to_wstring(glow);
        auto k = labelKeys.find(e);
        if (k != labelKeys.end() && k->second == key) {
            auto l = labels.find(e);
            if (l != labels.end()) return &l->second;
        }
        LabelDib d;
        if (!d.Build(e->name, IconLabelFont(), boxW, boxH, glow)) {
            labelKeys[e] = key;
            labels[e] = LabelDib{};
            return nullptr;
        }
        labelKeys[e] = key;
        auto r = labels.insert_or_assign(e, std::move(d));
        return &r.first->second;
    }

    void Clear() {
        icons.clear();
        labels.clear();
        labelKeys.clear();
    }
};

LRESULT CALLBACK DesktopSurface::WndProcStatic(HWND h, UINT m, WPARAM w, LPARAM l) {
    DesktopSurface* self = reinterpret_cast<DesktopSurface*>(GetWindowLongPtrW(h, GWLP_USERDATA));
    if (m == WM_NCCREATE) {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(l);
        self = reinterpret_cast<DesktopSurface*>(cs->lpCreateParams);
        SetWindowLongPtrW(h, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    }
    if (self) return self->WndProc(h, m, w, l);
    return DefWindowProcW(h, m, w, l);
}

void DesktopSurface::SetTextShadow(bool on) {
    if (textShadow_ == on) return;
    textShadow_ = on;
    if (impl_) {
        impl_->labels.clear();
        impl_->labelKeys.clear();
    }
    Redraw();
}

void DesktopSurface::InvalidateCache() {
    if (impl_) impl_->Clear();
    Redraw();
}

RECT DesktopSurface::CellRect(const DesktopIconView* e) const {
    RECT r{};
    r.left = e->pos.x - origin_.x;
    r.top = e->pos.y - origin_.y;
    r.right = r.left + cellW_;
    r.bottom = r.top + cellH_;
    return r;
}

DesktopIconView* DesktopSurface::HitTest(POINT pt) const {
    if (!icons_) return nullptr;
    for (auto* e : *icons_) {
        if (!e || !e->OnDesktop()) continue;
        RECT r = CellRect(e);
        if (pt.x >= r.left && pt.x < r.right && pt.y >= r.top && pt.y < r.bottom) return e;
    }
    return nullptr;
}

void DesktopSurface::ClearSelection() {
    if (!icons_) return;
    for (auto* e : *icons_) if (e) e->selected = false;
}

void DesktopSurface::SelectOnly(DesktopIconView* e) {
    ClearSelection();
    if (e) e->selected = true;
}

void DesktopSurface::ToggleSelection(DesktopIconView* e) {
    if (e) e->selected = !e->selected;
}

void DesktopSurface::UpdateMarqueeSelection() {
    if (!icons_) return;
    RECT m = Normalize(marqueeA_, marqueeB_);
    for (auto* e : *icons_) {
        if (!e || !e->OnDesktop()) continue;
        RECT r = CellRect(e);
        if (Intersects(r, m)) e->selected = true;
        else if (!marqueeAdditive_) e->selected = false;
    }
}

LRESULT DesktopSurface::WndProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_MOUSEACTIVATE:
        return MA_NOACTIVATE;

    case WM_ERASEBKGND:
        // 不透明路径由 WM_PAINT 自己画壁纸，别让系统再刷一遍底色
        return opaque_ ? 1 : 0;

    case WM_PAINT: {
        PAINTSTRUCT ps{};
        HDC dc = BeginPaint(h, &ps);
        if (dc) {
            if (opaque_) {
                PaintOpaque(dc);
            } else if (memDC_) {
                // 分层路径实际不靠 WM_PAINT，这里只做一次整窗更新
                EndPaint(h, &ps);
                Redraw();
                return 0;
            }
            EndPaint(h, &ps);
        }
        dirtyValid_ = false;
        return 0;
    }

    case WM_SETTINGCHANGE:
    case WM_THEMECHANGED:
    case WM_DISPLAYCHANGE:
        // 壁纸 / 主题 / 分辨率变了，整屏重画一遍壁纸
        if (opaque_) {
            dirtyValid_ = false;
            InvalidateRect(h, nullptr, FALSE);
        }
        return 0;

    case WM_LBUTTONDOWN: {
        POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        pressScreen_ = POINT{origin_.x + pt.x, origin_.y + pt.y};
        curScreen_ = pressScreen_;
        moved_ = false;
        dragSet_.clear();
        dragStartPos_.clear();

        const bool ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
        DesktopIconView* hit = HitTest(pt);

        if (hit) {
            if (ctrl) ToggleSelection(hit);
            else if (!hit->selected) SelectOnly(hit);
            if (hit->selected) {
                mode_ = Mode::Move;
                if (icons_) {
                    for (auto* e : *icons_) {
                        if (!e || !e->OnDesktop() || !e->selected) continue;
                        e->dragPos = e->pos;
                        dragSet_.push_back(e);
                        dragStartPos_.push_back(e->pos);
                    }
                }
            } else {
                mode_ = Mode::None;
            }
        } else {
            marqueeAdditive_ = ctrl;
            if (!ctrl) ClearSelection();
            mode_ = Mode::Marquee;
            marqueeA_ = marqueeB_ = pt;
        }
        SetCapture(h);
        Redraw();
        return 0;
    }

    case WM_MOUSEMOVE: {
        POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};

        if (mode_ == Mode::Move) {
            // 桌面层覆盖全屏且不移动，客户区坐标 + origin 就是屏幕坐标
            curScreen_ = POINT{origin_.x + pt.x, origin_.y + pt.y};
            int rawDx = curScreen_.x - pressScreen_.x;
            int rawDy = curScreen_.y - pressScreen_.y;
            if (std::abs(rawDx) > 3 || std::abs(rawDy) > 3) moved_ = true;

            bool over = overTarget && overTarget(curScreen_);
            int dx = over ? rawDx : SnapTo(rawDx, cellW_);
            int dy = over ? rawDy : SnapTo(rawDy, cellH_);

            if (!opaque_) Redraw();               // 分层路径要整屏重合成
            else for (auto* e : dragSet_) InvalidateCell(e, e->dragPos);  // 擦掉旧位置

            for (size_t i = 0; i < dragSet_.size(); ++i) {
                dragSet_[i]->dragPos = POINT{dragStartPos_[i].x + dx, dragStartPos_[i].y + dy};
            }
            if (opaque_) {
                for (auto* e : dragSet_) InvalidateCell(e, e->dragPos);
                FlushDirty();
            }
            if (onDragUpdate) onDragUpdate(dragSet_);
            return 0;
        }

        if (mode_ == Mode::Marquee) {
            if (opaque_) {
                // 旧框 + 新框（外扩一个格子，覆盖选中态的增减）
                RECT oldM = Normalize(marqueeA_, marqueeB_);
                InflateRect(&oldM, cellW_, cellH_);
                Dirty(oldM);
            }
            marqueeB_ = pt;
            UpdateMarqueeSelection();
            if (opaque_) {
                RECT newM = Normalize(marqueeA_, marqueeB_);
                InflateRect(&newM, cellW_, cellH_);
                Dirty(newM);
                FlushDirty();
            } else {
                Redraw();
            }
            return 0;
        }
        return 0;
    }

    case WM_LBUTTONUP: {
        POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        curScreen_ = POINT{origin_.x + pt.x, origin_.y + pt.y};
        if (GetCapture() == h) ReleaseCapture();
        if (onDragEnd) onDragEnd();

        if (mode_ == Mode::Move) {
            bool over = overTarget && overTarget(curScreen_);
            if (over && onDrop && !dragSet_.empty()) {
                if (onDrop(dragSet_, curScreen_)) {
                    for (auto* e : dragSet_) e->selected = false;
                    dragSet_.clear();
                    moved_ = false;
                    mode_ = Mode::None;
                    Redraw();
                    return 0;
                }
            }
            if (moved_ && !dragSet_.empty()) {
                for (auto* e : dragSet_) e->pos = e->dragPos;
                if (onMoved) onMoved(dragSet_);
            } else {
                for (auto* e : dragSet_) e->dragPos = e->pos;
            }
        }
        dragSet_.clear();
        dragStartPos_.clear();
        moved_ = false;
        mode_ = Mode::None;
        Redraw();
        return 0;
    }

    case WM_LBUTTONDBLCLK: {
        POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        DesktopIconView* item = HitTest(pt);
        if (GetCapture() == h) ReleaseCapture();
        dragSet_.clear();
        dragStartPos_.clear();
        moved_ = false;
        mode_ = Mode::None;
        if (item && onOpen) onOpen(item);
        return 0;
    }

    case WM_TIMER:
        if (wp == 1) {
            if (onTick) onTick();
            return 0;
        }
        break;

    case WM_DESTROY:
        return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

bool DesktopSurface::EnsureBackbuffer(int w, int h) {
    if (w <= 0 || h <= 0) return false;
    if (memDC_ && bufW_ == w && bufH_ == h) return true;

    if (memDC_) { DeleteDC(memDC_); memDC_ = nullptr; }
    if (dib_) { DeleteObject(dib_); dib_ = nullptr; }
    bits_ = nullptr;

    if (!CreateDib(w, h, memDC_, dib_, bits_)) {
        bufW_ = bufH_ = 0;
        return false;
    }
    bufW_ = w;
    bufH_ = h;
    return true;
}

void DesktopSurface::DrawOneLayered(DesktopIconView* e, bool dragging) {
    int x = (dragging ? e->dragPos.x : e->pos.x) - origin_.x;
    int y = (dragging ? e->dragPos.y : e->pos.y) - origin_.y;

    // 选中高亮（填充 + 1px 边框）
    if (e->selected) {
        RECT fill{x + 2, y + 2, x + cellW_ - 2, y + cellH_ - 2};
        FillRectAlpha(reinterpret_cast<BYTE*>(bits_), bufW_, bufH_, fill, 48, RGB(0, 120, 215));
        RECT edge[4] = {
            {fill.left, fill.top, fill.right, fill.top + 1},
            {fill.left, fill.bottom - 1, fill.right, fill.bottom},
            {fill.left, fill.top, fill.left + 1, fill.bottom},
            {fill.right - 1, fill.top, fill.right, fill.bottom},
        };
        for (const auto& r : edge)
            FillRectAlpha(reinterpret_cast<BYTE*>(bits_), bufW_, bufH_, r, 150, RGB(0, 120, 215));
    }

    if (!impl_) return;

    int iw = e->bmp.valid() ? e->bmp.width : 0;
    int ih = e->bmp.valid() ? e->bmp.height : 0;

    if (iw > 0) {
        // 直接用 IconBitmap 的像素（已经是预乘 BGRA），不再多占一份 IconDib 副本
        int ix = x + (cellW_ - iw) / 2;
        int iy = y + kIconTopPad;
        BlendInto(reinterpret_cast<BYTE*>(bits_), bufW_, bufH_,
                  reinterpret_cast<const BYTE*>(e->bmp.pixels.data()), iw, ih, ix, iy);
    }

    if (e->name.empty()) return;

    int textTop = y + kIconTopPad + (ih > 0 ? ih : iconPx_) + kTextGap;
    int textH = cellH_ - (kIconTopPad + (ih > 0 ? ih : iconPx_) + kTextGap);
    if (textH < 14) textH = 14;

    if (!impl_) return;
    LabelDib* lab = impl_->Label(e, cellW_ - kLabelInset, textH, textShadow_ ? 2 : 0);
    if (lab && lab->valid()) {
        int contentW = lab->w - lab->pad * 2;   // DIB 已按实际文字宽度收窄，按内容居中
        int left = x + (cellW_ - contentW) / 2 - lab->pad;
        BlendInto(reinterpret_cast<BYTE*>(bits_), bufW_, bufH_,
                  reinterpret_cast<const BYTE*>(lab->bits), lab->w, lab->h,
                  left, textTop - lab->pad);
    }
}

void DesktopSurface::PaintLayered() {
    if (!bits_ || !memDC_) return;
    std::memset(bits_, 0, (size_t)bufW_ * (size_t)bufH_ * 4);  // 全透明底

    if (!icons_) return;

    // 先画不动的，再画正在拖的，让拖动的图标压在上面
    for (auto* e : *icons_) {
        if (!e || !e->OnDesktop()) continue;
        bool inDrag = mode_ == Mode::Move && e->selected;
        if (!inDrag) DrawOneLayered(e, false);
    }
    // 正在拖的图标交给拖拽浮层画（桌面层在所有盒子下面，不然会被盒子挡住）

    if (mode_ == Mode::Marquee) {
        RECT m = Normalize(marqueeA_, marqueeB_);
        FillRectAlpha(reinterpret_cast<BYTE*>(bits_), bufW_, bufH_, m, 46, RGB(0, 120, 215));
        RECT edge[4] = {
            {m.left, m.top, m.right, m.top + 1},
            {m.left, m.bottom - 1, m.right, m.bottom},
            {m.left, m.top, m.left + 1, m.bottom},
            {m.right - 1, m.top, m.right, m.bottom},
        };
        for (const auto& r : edge)
            FillRectAlpha(reinterpret_cast<BYTE*>(bits_), bufW_, bufH_, r, 160, RGB(0, 120, 215));
    }

}

// ---------------------------------------------------------------------------
// 不透明路径
// ---------------------------------------------------------------------------

bool DesktopSurface::EnsureSolidDib() {
    if (solidDC_ && solidBits_) return true;
    if (!CreateDib(1, 1, solidDC_, solidBmp_, solidBits_)) return false;
    return true;
}

// 在任意 HDC 上做一次半透明纯色填充：把 1x1 纯色 DIB 拉伸过去，
// 用 SourceConstantAlpha 控透明度（不设 AC_SRC_ALPHA，走常量 alpha 通道）。
void DesktopSurface::FillRectBlend(HDC hdc, const RECT& r, int a, COLORREF color) {
    if (!hdc || a <= 0) return;
    if (a > 255) a = 255;
    int w = r.right - r.left;
    int h = r.bottom - r.top;
    if (w <= 0 || h <= 0) return;
    if (!EnsureSolidDib()) return;

    auto* p = reinterpret_cast<BYTE*>(solidBits_);
    p[0] = GetBValue(color);
    p[1] = GetGValue(color);
    p[2] = GetRValue(color);
    p[3] = 255;

    BLENDFUNCTION bf{};
    bf.BlendOp = AC_SRC_OVER;
    bf.SourceConstantAlpha = (BYTE)a;
    bf.AlphaFormat = 0;              // 用常量 alpha
    AlphaBlend(hdc, r.left, r.top, w, h, solidDC_, 0, 0, 1, 1, bf);
}

// 不透明路径：壁纸当背景（PaintDesktop 只填当前裁剪区），图标用 AlphaBlend 贴上去。
// 不需要任何整屏缓冲 —— 这就是省下约 29MB 的关键。
void DesktopSurface::PaintOpaque(HDC hdc) {
    if (!hdc) return;

    // 背景：把壁纸按裁剪区重画一遍。窗口铺满整个虚拟屏幕且原点对齐，
    // 所以贴图位置与真实桌面完全一致。
    if (!PaintDesktop(hdc)) {
        RECT cr{};
        GetClipBox(hdc, &cr);
        HBRUSH br = CreateSolidBrush(RGB(0, 0, 0));
        FillRect(hdc, &cr, br);
        DeleteObject(br);
    }

    if (icons_) {
        for (auto* e : *icons_) {
            if (!e || !e->OnDesktop()) continue;
            // 拖动中的图标交给拖拽浮层画（桌面层在所有盒子下面，会被挡住）
            if (mode_ == Mode::Move && e->selected) continue;
            DrawOneGdi(hdc, e, false);
        }
    }

    if (mode_ == Mode::Marquee) {
        RECT m = Normalize(marqueeA_, marqueeB_);
        FillRectBlend(hdc, m, 46, RGB(0, 120, 215));
        RECT edge[4] = {
            {m.left, m.top, m.right, m.top + 1},
            {m.left, m.bottom - 1, m.right, m.bottom},
            {m.left, m.top, m.left + 1, m.bottom},
            {m.right - 1, m.top, m.right, m.bottom},
        };
        for (const auto& r : edge) FillRectBlend(hdc, r, 160, RGB(0, 120, 215));
    }
}

void DesktopSurface::DrawOneGdi(HDC hdc, DesktopIconView* e, bool dragging) {
    int x = (dragging ? e->dragPos.x : e->pos.x) - origin_.x;
    int y = (dragging ? e->dragPos.y : e->pos.y) - origin_.y;

    if (e->selected) {
        RECT fill{x + 2, y + 2, x + cellW_ - 2, y + cellH_ - 2};
        FillRectBlend(hdc, fill, 48, RGB(0, 120, 215));
        RECT edge[4] = {
            {fill.left, fill.top, fill.right, fill.top + 1},
            {fill.left, fill.bottom - 1, fill.right, fill.bottom},
            {fill.left, fill.top, fill.left + 1, fill.bottom},
            {fill.right - 1, fill.top, fill.right, fill.bottom},
        };
        for (const auto& r : edge) FillRectBlend(hdc, r, 150, RGB(0, 120, 215));
    }

    if (!impl_) return;

    int ih = e->bmp.valid() ? e->bmp.height : 0;
    int iw = e->bmp.valid() ? e->bmp.width : 0;

    BLENDFUNCTION bf{};
    bf.BlendOp = AC_SRC_OVER;
    bf.SourceConstantAlpha = 255;
    bf.AlphaFormat = AC_SRC_ALPHA;   // 源是预乘 alpha → 边缘不会泛白

    if (iw > 0) {
        if (IconDib* d = impl_->Icon(e)) {
            if (d->valid()) {
                // 整数坐标 + 1:1 位块传输 → 不做任何重采样
                int ix = x + (cellW_ - d->w) / 2;
                int iy = y + kIconTopPad;
                AlphaBlend(hdc, ix, iy, d->w, d->h, d->dc, 0, 0, d->w, d->h, bf);
            }
        }
    }

    if (e->name.empty()) return;

    int textTop = y + kIconTopPad + (ih > 0 ? ih : iconPx_) + kTextGap;
    int textH = cellH_ - (kIconTopPad + (ih > 0 ? ih : iconPx_) + kTextGap);
    if (textH < 14) textH = 14;

    if (LabelDib* lab = impl_->Label(e, cellW_ - kLabelInset, textH, textShadow_ ? 2 : 0)) {
        if (lab->valid()) {
            int contentW = lab->w - lab->pad * 2;
            int left = x + (cellW_ - contentW) / 2 - lab->pad;
            AlphaBlend(hdc, left, textTop - lab->pad, lab->w, lab->h,
                       lab->dc, 0, 0, lab->w, lab->h, bf);
        }
    }
}

// ---------------------------------------------------------------------------
// 脏矩形
// ---------------------------------------------------------------------------

void DesktopSurface::Dirty(const RECT& r) {
    if (!hwnd_) return;
    RECT rr = r;
    if (dirtyValid_) {
        dirty_.left = std::min(dirty_.left, rr.left);
        dirty_.top = std::min(dirty_.top, rr.top);
        dirty_.right = std::max(dirty_.right, rr.right);
        dirty_.bottom = std::max(dirty_.bottom, rr.bottom);
    } else {
        dirty_ = rr;
        dirtyValid_ = true;
    }
    InvalidateRect(hwnd_, &rr, FALSE);
}

void DesktopSurface::FlushDirty() {
    if (!hwnd_) return;
    dirtyValid_ = false;
    dirty_ = RECT{0, 0, 0, 0};
    UpdateWindow(hwnd_);   // 立刻同步重绘，拖动才跟手
}

void DesktopSurface::InvalidateCell(const DesktopIconView* e, POINT pos) {
    if (!e) return;
    // 文字可能带光晕外扩，多留一点余量
    const int pad = 12;
    RECT r{pos.x - origin_.x - pad, pos.y - origin_.y - pad,
           pos.x - origin_.x + cellW_ + pad, pos.y - origin_.y + cellH_ + pad};
    Dirty(r);
}


bool DesktopSurface::Create() {
    impl_ = new Impl();

    // 默认走分层透明（唯一能正确透出「真实壁纸」的方式）。
    // MEOW_SURFACE=opaque 只适合壁纸是静态图片的机器：那条路用 PaintDesktop 现画壁纸，
    // 遇到动态壁纸（Spotlight / 壁纸软件）会画错，所以不设为默认。
    {
        wchar_t buf[32]{};
        DWORD n = GetEnvironmentVariableW(L"MEOW_SURFACE", buf, 32);
        if (n > 0 && _wcsicmp(buf, L"opaque") == 0) opaque_ = true;
    }

    HINSTANCE inst = GetModuleHandleW(nullptr);
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = WndProcStatic;
    wc.hInstance = inst;
    wc.lpszClassName = kClassName;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.style = CS_DBLCLKS;
    wc.hbrBackground = nullptr;   // 不透明路径自己画壁纸；分层路径整窗 alpha
    RegisterClassExW(&wc);

    int vx = GetSystemMetrics(SM_XVIRTUALSCREEN);
    int vy = GetSystemMetrics(SM_YVIRTUALSCREEN);
    int vw = GetSystemMetrics(SM_CXVIRTUALSCREEN);
    int vh = GetSystemMetrics(SM_CYVIRTUALSCREEN);
    if (vw <= 0 || vh <= 0) {
        RECT wa{};
        SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0);
        vx = wa.left; vy = wa.top;
        vw = wa.right - wa.left; vh = wa.bottom - wa.top;
    }
    origin_ = POINT{vx, vy};
    width_ = vw;
    height_ = vh;

    DWORD exStyle = WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE;
    if (!opaque_) exStyle |= WS_EX_LAYERED;
    hwnd_ = CreateWindowExW(
        exStyle, kClassName, L"", WS_POPUP,
        origin_.x, origin_.y, width_, height_,
        nullptr, nullptr, inst, this);
    if (!hwnd_) return false;

    if (!opaque_) {
        if (!EnsureBackbuffer(width_, height_)) {
            LogSurface("surface backbuffer FAILED err=%lu", GetLastError());
            return false;
        }
    }
    ShowWindow(hwnd_, SW_SHOWNOACTIVATE);
    Redraw();
    LogSurface("surface mode=%s %dx%d", opaque_ ? "opaque" : "layered", width_, height_);

    // 心跳：处理 Win+D 之类把桌面提到前面的情况，把层级摆回来
    SetTimer(hwnd_, 1, 500, nullptr);
    return true;
}

void DesktopSurface::Redraw() {
    if (!hwnd_) return;
    if (!opaque_) {
        if (!memDC_ || !bits_) return;
        PaintLayered();

        BLENDFUNCTION bf{};
        bf.BlendOp = AC_SRC_OVER;
        bf.SourceConstantAlpha = 255;
        bf.AlphaFormat = AC_SRC_ALPHA;

        HDC screen = GetDC(nullptr);
        POINT dst{origin_.x, origin_.y};
        SIZE size{width_, height_};
        POINT src{0, 0};
        UpdateLayeredWindow(hwnd_, screen, &dst, &size, memDC_, &src, 0, &bf, ULW_ALPHA);
        ReleaseDC(nullptr, screen);
        return;
    }
    // 不透明路径：全屏失效，WM_PAINT 里现画壁纸 + 图标
    dirtyValid_ = false;
    InvalidateRect(hwnd_, nullptr, FALSE);
    UpdateWindow(hwnd_);
}

void DesktopSurface::Destroy() {
    if (hwnd_) {
        KillTimer(hwnd_, 1);
        DestroyWindow(hwnd_);
        hwnd_ = nullptr;
    }
    if (dib_) { DeleteObject(dib_); dib_ = nullptr; }
    if (memDC_) { DeleteDC(memDC_); memDC_ = nullptr; }
    bits_ = nullptr;
    bufW_ = bufH_ = 0;
    if (solidDC_) { DeleteDC(solidDC_); solidDC_ = nullptr; }
    if (solidBmp_) { DeleteObject(solidBmp_); solidBmp_ = nullptr; }
    solidBits_ = nullptr;
    delete impl_;
    impl_ = nullptr;
}

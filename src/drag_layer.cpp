#include "drag_layer.h"

#include <algorithm>
#include <cstring>

#include "icon_render.h"

namespace {
const wchar_t* kClass = L"MeowDeskDrag";
const int kPad = 6;       // 浮层四周留白（给阴影/高亮）
}  // namespace

bool DragLayer::Create() {
    HINSTANCE inst = GetModuleHandleW(nullptr);
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = inst;
    wc.lpszClassName = kClass;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    RegisterClassExW(&wc);

    hwnd_ = CreateWindowExW(
        WS_EX_LAYERED | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_TRANSPARENT,
        kClass, L"", WS_POPUP, 0, 0, 1, 1, nullptr, nullptr, inst, nullptr);
    return hwnd_ != nullptr;
}

bool DragLayer::EnsureBuffer(int w, int h) {
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

void DragLayer::Present(int x, int y, int w, int h) {
    if (!hwnd_ || !memDC_) return;
    BLENDFUNCTION bf{};
    bf.BlendOp = AC_SRC_OVER;
    bf.SourceConstantAlpha = 255;
    bf.AlphaFormat = AC_SRC_ALPHA;

    HDC screen = GetDC(nullptr);
    POINT dst{x, y};
    SIZE size{w, h};
    POINT src{0, 0};
    UpdateLayeredWindow(hwnd_, screen, &dst, &size, memDC_, &src, 0, &bf, ULW_ALPHA);
    ReleaseDC(nullptr, screen);
}

void DragLayer::Show(const std::vector<DragGhost>& ghosts) {
    if (!hwnd_) return;

    int minX = 0, minY = 0, maxX = 0, maxY = 0;
    bool first = true;
    for (const auto& g : ghosts) {
        if (!g.bmp || !g.bmp->valid()) continue;
        int x1 = g.x, y1 = g.y;
        int x2 = x1 + g.bmp->width, y2 = y1 + g.bmp->height;
        if (first) {
            minX = x1; minY = y1; maxX = x2; maxY = y2;
            first = false;
        } else {
            minX = std::min(minX, x1); minY = std::min(minY, y1);
            maxX = std::max(maxX, x2); maxY = std::max(maxY, y2);
        }
    }
    if (first) { Hide(); return; }

    const int ox = minX - kPad;
    const int oy = minY - kPad;
    const int w = maxX - minX + kPad * 2;
    const int h = maxY - minY + kPad * 2;
    if (!EnsureBuffer(w, h)) return;

    std::memset(bits_, 0, (size_t)w * h * 4);
    auto* dst = reinterpret_cast<BYTE*>(bits_);

    for (const auto& g : ghosts) {
        if (!g.bmp || !g.bmp->valid()) continue;
        BlendInto(dst, w, h, g.bmp->pixels.data(), g.bmp->width, g.bmp->height,
                  g.x - ox, g.y - oy);
    }

    Present(ox, oy, w, h);
    if (!visible_) {
        ShowWindow(hwnd_, SW_SHOWNOACTIVATE);
        visible_ = true;
    }
}

void DragLayer::Hide() {
    if (hwnd_ && visible_) {
        ShowWindow(hwnd_, SW_HIDE);
        visible_ = false;
    }
}

void DragLayer::Destroy() {
    if (hwnd_) {
        DestroyWindow(hwnd_);
        hwnd_ = nullptr;
    }
    if (dib_) { DeleteObject(dib_); dib_ = nullptr; }
    if (memDC_) { DeleteDC(memDC_); memDC_ = nullptr; }
    bits_ = nullptr;
    bufW_ = bufH_ = 0;
    visible_ = false;
}

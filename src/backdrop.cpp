#include "backdrop.h"

#include <algorithm>
#include <cstring>

namespace {

// 单趟水平盒式模糊（滑动窗口求和）
void BoxBlurH(uint8_t* px, int w, int h, int r) {
    if (r <= 0) return;
    std::vector<uint32_t> sum(w * 4);
    for (int y = 0; y < h; ++y) {
        uint8_t* row = px + (size_t)y * w * 4;
        uint32_t acc[4] = {0, 0, 0, 0};
        const int win = 2 * r + 1;
        auto at = [&](int x) -> uint8_t* { return row + (size_t)std::clamp(x, 0, w - 1) * 4; };
        for (int x = -r; x <= r; ++x) {
            uint8_t* p = at(x);
            for (int c = 0; c < 4; ++c) acc[c] += p[c];
        }
        std::vector<uint8_t> line(w * 4);
        for (int x = 0; x < w; ++x) {
            for (int c = 0; c < 4; ++c) line[(size_t)x * 4 + c] = (uint8_t)(acc[c] / win);
            uint8_t* out = at(x - r);
            uint8_t* in = at(x + r + 1);
            for (int c = 0; c < 4; ++c) { acc[c] -= out[c]; acc[c] += in[c]; }
        }
        std::memcpy(row, line.data(), (size_t)w * 4);
    }
}

// 单趟垂直盒式模糊
void BoxBlurV(uint8_t* px, int w, int h, int r) {
    if (r <= 0) return;
    for (int x = 0; x < w; ++x) {
        uint32_t acc[4] = {0, 0, 0, 0};
        const int win = 2 * r + 1;
        auto at = [&](int y) -> uint8_t* {
            return px + (size_t)std::clamp(y, 0, h - 1) * w * 4 + (size_t)x * 4;
        };
        for (int y = -r; y <= r; ++y) {
            uint8_t* p = at(y);
            for (int c = 0; c < 4; ++c) acc[c] += p[c];
        }
        std::vector<uint8_t> line(h * 4);
        for (int y = 0; y < h; ++y) {
            for (int c = 0; c < 4; ++c) line[(size_t)y * 4 + c] = (uint8_t)(acc[c] / win);
            uint8_t* out = at(y - r);
            uint8_t* in = at(y + r + 1);
            for (int c = 0; c < 4; ++c) { acc[c] -= out[c]; acc[c] += in[c]; }
        }
        for (int y = 0; y < h; ++y) {
            std::memcpy(px + (size_t)y * w * 4 + (size_t)x * 4, &line[(size_t)y * 4], 4);
        }
    }
}

}  // namespace

bool CaptureBlurredBackdrop(int x, int y, int w, int h, int blurRadius,
                            std::vector<uint8_t>& out) {
    if (w <= 0 || h <= 0) return false;

    HDC screen = GetDC(nullptr);
    if (!screen) return false;
    HDC mem = CreateCompatibleDC(screen);

    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -h;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HBITMAP dib = CreateDIBSection(screen, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!dib || !bits) {
        if (dib) DeleteObject(dib);
        DeleteDC(mem);
        ReleaseDC(nullptr, screen);
        return false;
    }
    HGDIOBJ old = SelectObject(mem, dib);
    // CAPTUREBLT 让分层窗口也进画面（配合 WDA_EXCLUDEFROMCAPTURE 把自己排除）
    BOOL ok = BitBlt(mem, 0, 0, w, h, screen, x, y, SRCCOPY | CAPTUREBLT);

    out.assign((size_t)w * h * 4, 0);
    if (ok) std::memcpy(out.data(), bits, (size_t)w * h * 4);

    SelectObject(mem, old);
    DeleteObject(dib);
    DeleteDC(mem);
    ReleaseDC(nullptr, screen);
    if (!ok) return false;

    // 三次盒式模糊 ≈ 高斯
    int r = std::max(1, blurRadius);
    for (int pass = 0; pass < 3; ++pass) {
        BoxBlurH(out.data(), w, h, r);
        BoxBlurV(out.data(), w, h, r);
    }
    // 全部不透明
    for (size_t i = 3; i < out.size(); i += 4) out[i] = 255;
    return true;
}

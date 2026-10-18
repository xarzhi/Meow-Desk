#include "backdrop.h"

#include <algorithm>
#include <cstring>

namespace {

// 单趟水平盒式模糊（滑动窗口求和）
void BoxBlurH(uint8_t* px, int w, int h, int r) {
    if (r <= 0) return;
    for (int y = 0; y < h; ++y) {
        uint8_t* row = px + (size_t)y * w * 4;
        uint32_t acc[4] = {0, 0, 0, 0};
        const int win = 2 * r + 1;
        auto at = [&](int x) -> uint8_t* { return row + (size_t)std::clamp(x, 0, w - 1) * 4; };
        for (int x = -r; x <= r; ++x) {
            uint8_t* p = at(x);
            for (int c = 0; c < 4; ++c) acc[c] += p[c];
        }
        std::vector<uint8_t> line((size_t)w * 4);
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
        std::vector<uint8_t> line((size_t)h * 4);
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

struct MemDib {
    HDC dc = nullptr;
    HBITMAP bmp = nullptr;
    void* bits = nullptr;
    ~MemDib() {
        if (dc) DeleteDC(dc);
        if (bmp) DeleteObject(bmp);
    }
};

bool MakeMemDib(int w, int h, MemDib& m) {
    HDC screen = GetDC(nullptr);
    if (!screen) return false;
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -h;   // 自上而下
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HBITMAP dib = CreateDIBSection(screen, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!dib || !bits) {
        if (dib) DeleteObject(dib);
        ReleaseDC(nullptr, screen);
        return false;
    }
    HDC mem = CreateCompatibleDC(screen);
    ReleaseDC(nullptr, screen);
    if (!mem) { DeleteObject(dib); return false; }
    SelectObject(mem, dib);
    m.dc = mem;
    m.bmp = dib;
    m.bits = bits;
    return true;
}

}  // namespace

bool CaptureDesktopRegion(int x, int y, int w, int h, std::vector<uint8_t>& out) {
    out.clear();
    if (w <= 0 || h <= 0) return false;

    MemDib m;
    if (!MakeMemDib(w, h, m)) return false;

    // 经典用法：把视口原点平移到盒子所在的屏幕位置，PaintDesktop 就会把
    // 「这个位置上本该出现的壁纸」画进来。不需要任何屏幕捕获。
    SetViewportOrgEx(m.dc, -x, -y, nullptr);
    const BOOL ok = PaintDesktop(m.dc);
    SetViewportOrgEx(m.dc, 0, 0, nullptr);
    if (!ok) return false;

    out.assign((size_t)w * h * 4, 0);
    std::memcpy(out.data(), m.bits, out.size());
    for (size_t i = 3; i < out.size(); i += 4) out[i] = 255;

    // 全黑说明 PaintDesktop 返回成功但什么都没画
    bool any = false;
    for (size_t i = 0; i < out.size(); i += 4) {
        if (out[i] || out[i + 1] || out[i + 2]) { any = true; break; }
    }
    if (!any) { out.clear(); return false; }
    return true;
}

bool CaptureScreenRegion(int x, int y, int w, int h, std::vector<uint8_t>& out) {
    out.clear();
    if (w <= 0 || h <= 0) return false;

    MemDib m;
    if (!MakeMemDib(w, h, m)) return false;

    HDC screen = GetDC(nullptr);
    if (!screen) return false;
    // CAPTUREBLT 让分层窗口也进画面（配合 WDA_EXCLUDEFROMCAPTURE 把自己排除）
    const BOOL ok = BitBlt(m.dc, 0, 0, w, h, screen, x, y, SRCCOPY | CAPTUREBLT);
    ReleaseDC(nullptr, screen);
    if (!ok) return false;

    out.assign((size_t)w * h * 4, 0);
    std::memcpy(out.data(), m.bits, out.size());
    for (size_t i = 3; i < out.size(); i += 4) out[i] = 255;
    return true;
}

void BlurRgba(std::vector<uint8_t>& px, int w, int h, int radius, int passes) {
    if (w <= 0 || h <= 0 || radius <= 0) return;
    if (px.size() < (size_t)w * h * 4) return;
    const int r = std::max(1, radius);
    for (int p = 0; p < passes; ++p) {
        BoxBlurH(px.data(), w, h, r);
        BoxBlurV(px.data(), w, h, r);
    }
    for (size_t i = 3; i < px.size(); i += 4) px[i] = 255;
}

bool NearlyUniform(const std::vector<uint8_t>& px, int w, int h) {
    if (px.size() < (size_t)w * h * 4) return true;
    int mn[3] = {255, 255, 255}, mx[3] = {0, 0, 0};
    for (size_t i = 0; i < px.size(); i += 4) {
        for (int c = 0; c < 3; ++c) {
            int v = px[i + c];
            if (v < mn[c]) mn[c] = v;
            if (v > mx[c]) mx[c] = v;
        }
    }
    return (mx[0] - mn[0]) <= 3 && (mx[1] - mn[1]) <= 3 && (mx[2] - mn[2]) <= 3;
}

bool CaptureBlurredBackdrop(int x, int y, int w, int h, int blurRadius,
                            std::vector<uint8_t>& out, bool preferDesktop) {
    bool ok = false;
    if (preferDesktop) {
        ok = CaptureDesktopRegion(x, y, w, h, out);
        if (!ok) ok = CaptureScreenRegion(x, y, w, h, out);
    } else {
        // 首选抓屏：动态壁纸（Wallpaper Engine 等）只有这条路能拿到真实画面，
        // 调用方需先把本窗口 SetWindowDisplayAffinity(WDA_EXCLUDEFROMCAPTURE) 排除掉
        ok = CaptureScreenRegion(x, y, w, h, out);
        if (!ok) ok = CaptureDesktopRegion(x, y, w, h, out);
    }
    if (!ok) return false;
    BlurRgba(out, w, h, blurRadius);
    return true;
}

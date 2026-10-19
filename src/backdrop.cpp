#include "backdrop.h"

#include <algorithm>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <mutex>
#include <thread>

namespace {

// 单趟水平盒式模糊（滑动窗口求和）
void BoxBlurH(uint8_t* px, int w, int h, int r) {
    if (r <= 0) return;
    std::vector<uint8_t> line((size_t)w * 4);   // 复用：以前每行都 new 一次
    for (int y = 0; y < h; ++y) {
        uint8_t* row = px + (size_t)y * w * 4;
        uint32_t acc[4] = {0, 0, 0, 0};
        const int win = 2 * r + 1;
        auto at = [&](int x) -> uint8_t* { return row + (size_t)std::clamp(x, 0, w - 1) * 4; };
        for (int x = -r; x <= r; ++x) {
            uint8_t* p = at(x);
            for (int c = 0; c < 4; ++c) acc[c] += p[c];
        }
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
    std::vector<uint8_t> line((size_t)h * 4);   // 复用：以前每列都 new 一次
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

// ---- 1/4 缩放的模糊流水线（见 BlurFastRgba）----
constexpr int kBlurScale = 4;

// 4x4 块平均缩小
void Downsample4(const uint8_t* src, int w, int h, std::vector<uint8_t>& out, int& ow,
                 int& oh) {
    ow = (w + kBlurScale - 1) / kBlurScale;
    oh = (h + kBlurScale - 1) / kBlurScale;
    out.assign((size_t)ow * oh * 4, 255);
    for (int y = 0; y < oh; ++y) {
        const int sy0 = y * kBlurScale;
        const int sy1 = std::min(h, sy0 + kBlurScale);
        for (int x = 0; x < ow; ++x) {
            const int sx0 = x * kBlurScale;
            const int sx1 = std::min(w, sx0 + kBlurScale);
            uint32_t acc[3] = {0, 0, 0};
            for (int sy = sy0; sy < sy1; ++sy) {
                const uint8_t* p = src + ((size_t)sy * w + sx0) * 4;
                for (int sx = sx0; sx < sx1; ++sx, p += 4) {
                    acc[0] += p[0];
                    acc[1] += p[1];
                    acc[2] += p[2];
                }
            }
            const uint32_t n = (uint32_t)((sy1 - sy0) * (sx1 - sx0));
            uint8_t* q = &out[((size_t)y * ow + x) * 4];
            q[0] = (uint8_t)(acc[0] / n);
            q[1] = (uint8_t)(acc[1] / n);
            q[2] = (uint8_t)(acc[2] / n);
            q[3] = 255;
        }
    }
}

// 双线性放大回原尺寸。权重一律用 1024 定点（浮点 + lround 的写法实测比模糊本身还慢）。
void UpsampleTo(const uint8_t* src, int sw, int sh, uint8_t* dst, int dw, int dh) {
    std::vector<int> cx0(dw), cx1(dw), cw(dw);
    for (int x = 0; x < dw; ++x) {
        // ((x + 0.5) / scale - 0.5) * 1024
        const int f = ((x * 2 + 1) * (1024 / (kBlurScale * 2))) - 512;
        const int i0 = f >> 10;
        cx0[x] = std::clamp(i0, 0, sw - 1);
        cx1[x] = std::clamp(i0 + 1, 0, sw - 1);
        cw[x] = f & 1023;
    }
    for (int y = 0; y < dh; ++y) {
        const int f = ((y * 2 + 1) * (1024 / (kBlurScale * 2))) - 512;
        const int i0 = f >> 10;
        const int r0 = std::clamp(i0, 0, sh - 1);
        const int r1 = std::clamp(i0 + 1, 0, sh - 1);
        const int wy = f & 1023;
        const uint8_t* p0 = src + (size_t)r0 * sw * 4;
        const uint8_t* p1 = src + (size_t)r1 * sw * 4;
        uint8_t* o = dst + (size_t)y * dw * 4;
        for (int x = 0; x < dw; ++x) {
            const uint8_t* a = p0 + (size_t)cx0[x] * 4;
            const uint8_t* b = p0 + (size_t)cx1[x] * 4;
            const uint8_t* c = p1 + (size_t)cx0[x] * 4;
            const uint8_t* d = p1 + (size_t)cx1[x] * 4;
            const int wx = cw[x];
            const int ix = 1024 - wx;
            const int iy = 1024 - wy;
            for (int k = 0; k < 3; ++k) {
                const int top = (a[k] * ix + b[k] * wx) >> 10;
                const int bot = (c[k] * ix + d[k] * wx) >> 10;
                o[(size_t)x * 4 + k] = (uint8_t)((top * iy + bot * wy) >> 10);
            }
            o[(size_t)x * 4 + 3] = 255;
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

// 不带 CAPTUREBLT：分层窗口天然不进画面，省掉 WDA_EXCLUDEFROMCAPTURE 那一套
// （那一步每次都会让 DWM 重新合成桌面，拖动时就是肉眼可见的卡顿）。详见头文件注释。
bool CaptureScreenRegionFast(int x, int y, int w, int h, std::vector<uint8_t>& out) {
    out.clear();
    if (w <= 0 || h <= 0) return false;

    MemDib m;
    if (!MakeMemDib(w, h, m)) return false;

    HDC screen = GetDC(nullptr);
    if (!screen) return false;
    const BOOL ok = BitBlt(m.dc, 0, 0, w, h, screen, x, y, SRCCOPY);
    ReleaseDC(nullptr, screen);
    if (!ok) return false;

    out.assign((size_t)w * h * 4, 0);
    std::memcpy(out.data(), m.bits, out.size());
    for (size_t i = 3; i < out.size(); i += 4) out[i] = 255;
    return true;
}

bool SampleScreenMean(int x, int y, int w, int h, int& r, int& g, int& b) {
    r = g = b = 0;
    if (w <= 0 || h <= 0) return false;

    // 16×16 就够了：当指纹只关心「这块地方整体变色没有」，
    // 分辨率再高也只会把噪点和抗锯齿抖动一起放大。
    const int dw = 16, dh = 16;
    MemDib m;
    if (!MakeMemDib(dw, dh, m)) return false;

    HDC screen = GetDC(nullptr);
    if (!screen) return false;
    // COLORONCOLOR（近邻取样）而不是 HALFTONE：后者每次调用都要建一张半色调表，
    // 这一路是「每 500ms 每个盒子一次」的常驻开销，不能用慢模式。
    // 当指纹只关心均值，近邻取样的 256 个点完全够。
    SetStretchBltMode(m.dc, COLORONCOLOR);
    // 不带 CAPTUREBLT：分层窗口不进画面（和我们抓底图的快路径保持同一层视角，
    // 否则量到的是我们自己画出来的东西，永远量不出「壁纸变了」）
    const BOOL ok = StretchBlt(m.dc, 0, 0, dw, dh, screen, x, y, w, h, SRCCOPY);
    ReleaseDC(nullptr, screen);
    if (!ok) return false;

    const auto* p = reinterpret_cast<const uint8_t*>(m.bits);
    long long sr = 0, sg = 0, sb = 0;
    for (int i = 0; i < dw * dh; ++i) {
        sb += p[i * 4 + 0];
        sg += p[i * 4 + 1];
        sr += p[i * 4 + 2];
    }
    const int n = dw * dh;
    r = (int)(sr / n);
    g = (int)(sg / n);
    b = (int)(sb / n);
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

void BlurFastRgba(std::vector<uint8_t>& px, int w, int h, int radius, int passes) {
    if (w <= 0 || h <= 0 || radius <= 0) return;
    if (px.size() < (size_t)w * h * 4) return;
    if (w < kBlurScale * 4 || h < kBlurScale * 4) {   // 太小就别折腾了，直接原尺寸糊
        BlurRgba(px, w, h, radius, passes);
        return;
    }

    std::vector<uint8_t> tiny;
    int tw = 0, th = 0;
    Downsample4(px.data(), w, h, tiny, tw, th);

    // 缩小 4 倍之后，同样的视觉模糊半径只要 1/4
    const int r = std::max(1, radius / kBlurScale);
    for (int p = 0; p < passes; ++p) {
        BoxBlurH(tiny.data(), tw, th, r);
        BoxBlurV(tiny.data(), tw, th, r);
    }

    UpsampleTo(tiny.data(), tw, th, px.data(), w, h);
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
    BlurFastRgba(out, w, h, blurRadius);
    return true;
}

bool CaptureBlurredBackdropFast(int x, int y, int w, int h, std::vector<uint8_t>& out,
                                bool* uniform) {
    if (uniform) *uniform = false;
    if (!CaptureScreenRegionFast(x, y, w, h, out)) return false;
    // 分层窗口画的壁纸（某些动态壁纸）在不带 CAPTUREBLT 的抓屏里是「空的」，
    // 拿到的是一片纯色甚至全黑 —— 但也可能壁纸本身就是纯色。这里只报事实。
    const bool uni = NearlyUniform(out, w, h);
    if (uniform) *uniform = uni;
    BlurFastRgba(out, w, h, 20);
    return true;
}

// ---------------------------------------------------------------------------
// 后台抓取线程
//
// 故意做成「永不析构」的单例：抓屏线程如果在静态析构阶段还在跑 BitBlt，
// 会和 GDI 的卸载撞上。进程退出时让系统回收就行（一次性的几百字节）。
// ---------------------------------------------------------------------------
namespace {

struct JobRequest {
    RECT rect{};
    std::shared_ptr<BackdropCaptureJob> job;
};

struct Worker {
    std::mutex mu;
    std::condition_variable cv;
    std::deque<JobRequest> q;
    std::thread th;
    bool started = false;

    void Start() {
        if (started) return;
        started = true;
        th = std::thread([this] { Loop(); });
        th.detach();   // 见上面：故意不 join，进程结束时由系统收尾
    }

    void Loop() {
        for (;;) {
            JobRequest req;
            {
                std::unique_lock<std::mutex> lk(mu);
                cv.wait(lk, [this] { return !q.empty(); });
                req = std::move(q.front());
                q.pop_front();
            }
            auto& j = *req.job;
            const int x = (int)req.rect.left;
            const int y = (int)req.rect.top;
            const int w = (int)(req.rect.right - req.rect.left);
            const int h = (int)(req.rect.bottom - req.rect.top);
            j.x = x;
            j.y = y;
            j.w = w;
            j.h = h;
            bool uni = false;
            j.ok = CaptureBlurredBackdropFast(x, y, w, h, j.px, &uni);
            j.uniform = uni;
            j.finished.store(true, std::memory_order_release);
        }
    }

    bool Push(JobRequest req) {
        {
            std::lock_guard<std::mutex> lk(mu);
            if (q.size() >= 4) return false;   // 拖得快，攒着没意义
            q.push_back(std::move(req));
        }
        cv.notify_one();
        return true;
    }
};

Worker& GetWorker() {
    static Worker* w = new Worker();   // 有意泄漏
    return *w;
}

}  // namespace

std::shared_ptr<BackdropCaptureJob> BackdropCaptureAsync(const RECT& rect) {
    if (rect.right - rect.left < 16 || rect.bottom - rect.top < 16) return nullptr;
    Worker& w = GetWorker();
    w.Start();
    auto job = std::make_shared<BackdropCaptureJob>();
    job->rect = rect;
    if (!w.Push(JobRequest{rect, job})) {
        // 队列积压：直接当作「这次没抓成」，调用方下一帧会重新提交最近的矩形
        job->finished.store(true, std::memory_order_release);
    }
    return job;
}

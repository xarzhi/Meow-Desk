#include "backdrop.h"

#include "screen_util.h"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <mutex>
#include <thread>

namespace {

// ---------------------------------------------------------------------------
// 抓屏排除自家窗口
//
// 【血的教训】以前这里写着「不带 CAPTUREBLT 时分层窗口天然不进画面」，据此做了一条
// 「快路径」：不碰任何窗口的 WDA，直接 BitBlt。这句话在本机（Win11 22621）上是错的，
// 用一个 8×8 的分层小红块验证过：普通 BitBlt 抓到的均值 R = 164（壁纸 152，
// 红块面积按比例算出来 163.4），也就是**分层窗口照样被抓进来**；而带上
// WDA_EXCLUDEFROMCAPTURE 之后回到 152，CAPTUREBLT 与否结果逐字节相同。
//
// 于是「快路径」抓到的全是自己：盒子的 per-pixel alpha 缓冲、桌面图标层（整屏）
// 全在里面。磨砂底图 = 把上一帧的自己再糊一遍，越糊越暗，而且盒子以外的东西
// 变化根本传不进来 —— 用户看到的就是「亚克力永远不对、后面变八百年了它也不变」。
//
// 所以现在只有一条抓屏路径：先排除，再抓，再还原。WDA 只对调用进程自己的窗口有效
// （别的进程调会返回 FALSE），这也是为什么排除必须做在本进程里。
// ---------------------------------------------------------------------------
CaptureExcludeFn g_excludeHook = nullptr;
CaptureLogFn g_logHook = nullptr;
bool g_loggedNoWda = false;
// 一把锁把整段「排除 → 抓屏 → 还原」串起来：后台线程和 UI 线程都会抓，
// 交错的话 A 的还原会把 B 排除掉的窗口又放回画面里。
std::mutex g_exclMu;

void LogOnce(const char* msg) {
    if (!g_logHook || g_loggedNoWda) return;
    g_loggedNoWda = true;
    g_logHook(msg);
}

// 作用域内把自家窗口从抓屏画面里摘掉。
struct ExclGuard {
    std::unique_lock<std::mutex> lk{g_exclMu};
    RECT rc{};
    HWND self = nullptr;
    bool hooked = false;
    bool selfOk = false;

    ExclGuard(const RECT& r, HWND s) : rc(r), self(s) {
        if (self) selfOk = SetWindowDisplayAffinity(self, WDA_EXCLUDEFROMCAPTURE) != 0;
        if (g_excludeHook) {
            g_excludeHook(rc, true);
            hooked = true;
        }
        if (self && !selfOk) {
            LogOnce("backdrop: WDA_EXCLUDEFROMCAPTURE failed -> PaintDesktop fallback");
        }
    }
    ~ExclGuard() {
        if (hooked) g_excludeHook(rc, false);
        if (self && selfOk) SetWindowDisplayAffinity(self, WDA_NONE);
    }
    // 抓屏能不能用：排除不掉自己（老系统 / 特殊会话）就只能走 PaintDesktop。
    bool ok() const { return selfOk; }
};

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
                            std::vector<uint8_t>& out, bool preferDesktop, HWND self) {
    bool ok = false;
    {
        // 先排除、再抓、出作用域还原（见 ExclGuard 的注释）。模糊放在锁外面：
        // 那是纯内存运算，占着锁会让另一个线程的抓屏白等。
        ExclGuard g(RECT{x, y, x + w, y + h}, self);
        const bool canBlit = g.ok();
        if (preferDesktop || !canBlit) {
            ok = CaptureDesktopRegion(x, y, w, h, out);
            if (!ok && canBlit) ok = CaptureScreenRegion(x, y, w, h, out);
        } else {
            // 首选抓屏：动态壁纸（Wallpaper Engine 等）只有这条路能拿到真实画面
            ok = CaptureScreenRegion(x, y, w, h, out);
            if (!ok) ok = CaptureDesktopRegion(x, y, w, h, out);
        }
    }
    if (!ok) return false;
    BlurFastRgba(out, w, h, blurRadius);
    return true;
}

bool CaptureBlurredBackdropFast(int x, int y, int w, int h, std::vector<uint8_t>& out,
                                bool* uniform, HWND self) {
    if (uniform) *uniform = false;
    bool ok = false;
    {
        ExclGuard g(RECT{x, y, x + w, y + h}, self);
        // 排除不掉自己就别抓了：那抓回来的必然是我们自己上一帧的画面，
        // 还不如只要壁纸（PaintDesktop 天生看不见窗口）。
        ok = g.ok() ? CaptureScreenRegionFast(x, y, w, h, out)
                    : CaptureDesktopRegion(x, y, w, h, out);
    }
    if (!ok) return false;
    // 除了我们自己之外，整块画面是一片纯色 —— 可能是壁纸本来就纯色，也可能是
    // 壁纸画在别人家的分层窗口上（我们排除不掉它，抓到的是它下面那层）。只报事实。
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
    HWND self = nullptr;
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
            j.ok = CaptureBlurredBackdropFast(x, y, w, h, j.px, &uni, req.self);
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

void SetCaptureExcludeHook(CaptureExcludeFn fn) {
    std::lock_guard<std::mutex> lk(g_exclMu);
    g_excludeHook = fn;
}

void SetCaptureLogHook(CaptureLogFn fn) {
    std::lock_guard<std::mutex> lk(g_exclMu);
    g_logHook = fn;
}

std::shared_ptr<BackdropCaptureJob> BackdropCaptureAsync(const RECT& rect, HWND self) {
    if (rect.right - rect.left < 16 || rect.bottom - rect.top < 16) return nullptr;
    Worker& w = GetWorker();
    w.Start();
    auto job = std::make_shared<BackdropCaptureJob>();
    job->rect = rect;
    if (!w.Push(JobRequest{rect, self, job})) {
        // 队列积压：直接当作「这次没抓成」，调用方下一帧会重新提交最近的矩形
        job->finished.store(true, std::memory_order_release);
    }
    return job;
}

// ---------------------------------------------------------------------------
// 磨砂帧泵
//
// 一次抓取的成本几乎全是「从 DWM 合成表面回读」的开销（见头文件实测表），
// 所以：所有盒子共用一张，各自从里面裁。7 个盒子各自抓 = 7 × 6.4ms ≈ 45ms，
// 共用一张 = 一次 8~20ms，**总量反而更少**，而且不用排队、不用每个盒子等一帧。
// 抓多大由主程序给（所有可见磨砂盒子的并集），不是无脑整屏。
// ---------------------------------------------------------------------------
namespace {

// 帧的像素密度：1/4。和 BlurFastRgba 内部那一步是同一个刻度
// （缩小 4 倍之后半径 / 4 就是同样的视觉效果），所以内容一致、内存只剩 1/16。
constexpr int kFrameScale = kBlurScale;
constexpr int kFrameBlurR = 20 / kFrameScale;   // 5

struct FramePump {
    std::mutex mu;
    std::condition_variable cv;
    std::shared_ptr<const BackdropFrame> cur;
    unsigned long long serial = 0;
    RECT want{};              // 需要磨砂的屏幕区域（所有磨砂盒子的并集）
    bool wantValid = false;   // false = 没人需要，泵彻底停手
    bool paused = false;      // 正在拖 / 改尺寸
    bool started = false;
    DWORD intervalMs = 150;
    // MEOW_BACKDROP=desktop 的调试口子：只要壁纸、不要窗口（某些动态壁纸自己
    // 也是分层窗口，排除不掉，抓回来是它下面那一层 —— 那就只能退回 PaintDesktop）
    bool preferDesktop = false;
    DWORD uiTid = 0;
    UINT wake = 0;
    // 只给泵线程用的复用缓冲：抓取区域的原图（按并集大小）。
    // 每帧重新 new 一次是纯粹的内存抖动，没必要 —— 但也不主动留着，
    // 区域缩小 / 需求消失时要放掉。
    //
    // 【只归泵线程用】rawBuf 只能由泵线程读写：UI 线程要它释放时只置
    // releaseBuf，由泵线程在下一轮开头自己 swap 掉。否则就是
    // 「UI 线程正在 swap、泵线程正在往里 BitBlt」的悬空指针。
    std::vector<uint8_t> rawBuf;
    bool releaseBuf = false;
};

FramePump& Pump() {
    static FramePump* p = new FramePump();   // 有意泄漏，理由同 Worker
    return *p;
}

// 抓取区域的原图 → 1/4 缩放 + 三次盒式模糊（≈ 全分辨率半径 20 的高斯）。
// 直接写进 out（也就是帧自己的 px），中间不落第二份 1/4 图。
void MakeFramePixels(const std::vector<uint8_t>& raw, int w, int h, std::vector<uint8_t>& out,
                     int& ow, int& oh) {
    Downsample4(raw.data(), w, h, out, ow, oh);
    if (ow <= 0 || oh <= 0) {
        out.clear();
        return;
    }
    for (int p = 0; p < 3; ++p) {
        BoxBlurH(out.data(), ow, oh, kFrameBlurR);
        BoxBlurV(out.data(), ow, oh, kFrameBlurR);
    }
}

// 只在泵线程里调用。region 是「需要磨砂的区域」，按 4 对齐向外取整后抓取。
std::shared_ptr<BackdropFrame> GrabRegion(FramePump& p, const RECT& region) {
    const RECT vs = ScreenUtil::VirtualScreen();
    // 按 scale 对齐（floor left/top，ceil right/bottom），这样帧的原点不会因为
    // 盒子挪了 1px 就变，缓存判断才稳。C++ 的 % 对负数给负余数，这里统一掰成正的。
    auto floorTo = [](int v, int s) { return v - ((v % s) + s) % s; };
    auto ceilTo = [](int v, int s) { return v + ((s - v % s) % s); };

    int x0 = std::max((int)vs.left, floorTo((int)region.left, kFrameScale));
    int y0 = std::max((int)vs.top, floorTo((int)region.top, kFrameScale));
    int x1 = std::min((int)vs.right, ceilTo((int)region.right, kFrameScale));
    int y1 = std::min((int)vs.bottom, ceilTo((int)region.bottom, kFrameScale));
    if (x1 - x0 < 32 || y1 - y0 < 32) return nullptr;

    // 需求区域比屏幕还大（盒子摊得很开）就干脆整屏，少一次边界判断
    const long long fullArea = (long long)(vs.right - vs.left) * (vs.bottom - vs.top);
    const long long area = (long long)(x1 - x0) * (y1 - y0);
    if (area * 10 >= fullArea * 7) {
        x0 = (int)vs.left;
        y0 = (int)vs.top;
        x1 = (int)vs.right;
        y1 = (int)vs.bottom;
    }

    const int w = x1 - x0, h = y1 - y0;
    const RECT rc{x0, y0, x1, y1};
    bool ok = false;
    {
        // self 为空 = 一次抓一片区域（没有「某一个盒子」可以排除，
        // 靠排除钩子把自家窗口全从画面里摘掉）
        ExclGuard g(rc, nullptr);
        ok = (!p.preferDesktop && g.hooked) ? CaptureScreenRegionFast(x0, y0, w, h, p.rawBuf)
                                            : CaptureDesktopRegion(x0, y0, w, h, p.rawBuf);
    }
    if (!ok || p.rawBuf.size() < (size_t)w * (size_t)h * 4) return nullptr;

    auto f = std::make_shared<BackdropFrame>();
    f->x = x0;
    f->y = y0;
    f->w = w;
    f->h = h;
    f->scale = kFrameScale;
    int ow = 0, oh = 0;
    MakeFramePixels(p.rawBuf, w, h, f->px, ow, oh);
    // 区域缩小了很多（比如从整屏变成一个小盒子）：别一直占着那块大缓冲
    if (p.rawBuf.capacity() > (size_t)w * (size_t)h * 4 * 2) {
        std::vector<uint8_t>().swap(p.rawBuf);
    }
    if (f->px.size() != (size_t)f->pixW() * (size_t)f->pixH() * 4 || ow != f->pixW() ||
        oh != f->pixH())
        return nullptr;
    return f;
}

void PumpLoop() {
    FramePump& p = Pump();
    for (;;) {
        RECT region{};
        {
            std::unique_lock<std::mutex> lk(p.mu);
            // 释放缓冲只在泵线程做（rawBuf 是泵线程的私有工作区）
            if (p.releaseBuf) {
                p.releaseBuf = false;
                std::vector<uint8_t>().swap(p.rawBuf);
            }
            p.cv.wait(lk, [&p] { return p.wantValid && !p.paused; });
            region = p.want;
        }
        auto f = GrabRegion(p, region);   // 3~20ms，全程不持锁
        DWORD tid = 0;
        UINT msg = 0;
        if (f) {
            std::lock_guard<std::mutex> lk(p.mu);
            f->serial = ++p.serial;
            p.cur = f;
            tid = p.uiTid;
            msg = p.wake;
        }
        if (tid && msg) PostThreadMessageW(tid, msg, 0, 0);
        // 节流。被「不需要了 / 暂停」唤醒就立刻回去等，不白抓。
        std::unique_lock<std::mutex> lk(p.mu);
        p.cv.wait_for(lk, std::chrono::milliseconds(p.intervalMs),
                      [&p] { return !p.wantValid || p.paused; });
    }
}

}  // namespace

void InitBackdropFramePump(DWORD uiThreadId, UINT wakeMsg) {
    FramePump& p = Pump();
    {
        std::lock_guard<std::mutex> lk(p.mu);
        p.uiTid = uiThreadId;
        p.wake = wakeMsg;
        if (p.started) return;
        p.started = true;
    }
    // 调试口子：MEOW_BD_MS=1000 把刷新放慢，方便数到底刷了几帧 / 对比卡不卡
    wchar_t v[32]{};
    if (GetEnvironmentVariableW(L"MEOW_BD_MS", v, 32) > 0) {
        const long ms = wcstol(v, nullptr, 10);
        if (ms >= 30 && ms <= 5000) p.intervalMs = (DWORD)ms;
    }
    // 和 RefreshBackdrop 里那条 MEOW_BACKDROP=desktop 保持同一语义
    wchar_t mode[32]{};
    if (GetEnvironmentVariableW(L"MEOW_BACKDROP", mode, 32) > 0) {
        p.preferDesktop = _wcsicmp(mode, L"desktop") == 0;
    }
    std::thread th(PumpLoop);
    th.detach();   // 同 Worker：故意不 join，进程退出时由系统收尾
}

void SetBackdropFrameRegion(const RECT* region) {
    FramePump& p = Pump();
    {
        std::lock_guard<std::mutex> lk(p.mu);
        if (!region) {
            if (!p.wantValid) return;
            p.wantValid = false;
            // 需求没了就把那块原图放掉（整屏时是 14.7MB，凭这一点不能留着）。
            // 不能在这里直接 swap：rawBuf 归泵线程所有，它可能正在往里 BitBlt。
            p.releaseBuf = true;
        } else {
            if (p.wantValid && EqualRect(&p.want, region)) return;
            p.want = *region;
            p.wantValid = true;
        }
    }
    p.cv.notify_all();
}

void SetBackdropFramePaused(bool paused) {
    FramePump& p = Pump();
    {
        std::lock_guard<std::mutex> lk(p.mu);
        if (p.paused == paused) return;
        p.paused = paused;
    }
    p.cv.notify_all();
}

void SetBackdropFrameInterval(DWORD ms) {
    if (ms < 30 || ms > 5000) return;
    FramePump& p = Pump();
    {
        std::lock_guard<std::mutex> lk(p.mu);
        if (p.intervalMs == ms) return;
        p.intervalMs = ms;
    }
    p.cv.notify_all();
}

std::shared_ptr<const BackdropFrame> CurrentBackdropFrame() {
    FramePump& p = Pump();
    std::lock_guard<std::mutex> lk(p.mu);
    return p.cur;
}

bool CropBackdropFrame(const BackdropFrame& f, const RECT& want, std::vector<uint8_t>& out,
                       RECT& covered) {
    out.clear();
    const int s = f.scale > 0 ? f.scale : 2;
    const int pw = f.pixW(), ph = f.pixH();
    if (pw <= 0 || ph <= 0 || f.px.size() < (size_t)pw * (size_t)ph * 4) return false;

    const int x0 = std::max((int)want.left, f.x);
    const int y0 = std::max((int)want.top, f.y);
    const int x1 = std::min((int)want.right, f.x + f.w);
    const int y1 = std::min((int)want.bottom, f.y + f.h);
    if (x1 - x0 < 8 || y1 - y0 < 8) return false;

    // 按 s 对齐切整块：源图是按 s 分块的，切半块等于错位半像素
    const int cx0 = (x0 - f.x) / s;
    const int cy0 = (y0 - f.y) / s;
    const int cx1 = std::min(pw, (x1 - f.x + s - 1) / s);
    const int cy1 = std::min(ph, (y1 - f.y + s - 1) / s);
    if (cx1 <= cx0 || cy1 <= cy0) return false;

    const int cw = cx1 - cx0, ch = cy1 - cy0;
    out.resize((size_t)cw * (size_t)ch * 4);
    for (int y = 0; y < ch; ++y) {
        std::memcpy(out.data() + (size_t)y * (size_t)cw * 4,
                    f.px.data() + ((size_t)(cy0 + y) * (size_t)pw + cx0) * 4,
                    (size_t)cw * 4);
    }
    covered.left = f.x + cx0 * s;
    covered.top = f.y + cy0 * s;
    covered.right = f.x + cx1 * s;
    covered.bottom = f.y + cy1 * s;
    return true;
}

// 亚克力底图：取得盒子背后那块壁纸，再做高斯近似模糊
//
// 为什么要自己实现：客户端的「Win11 亚克力」有两种官方途径，但在我们的场景里都不可用
//   1) SetWindowCompositionAttribute(ACCENT_ENABLE_ACRYLICBLURBEHIND)
//      —— 只对「非分层」窗口生效。盒子是 UpdateLayeredWindow 的 per-pixel alpha 窗口，
//         调用后没有任何效果。
//   2) DwmEnableBlurBehindWindow
//      —— Win8 以后只给一层磨砂玻璃的灰底，不模糊背后的内容。
//
// 所以自己抓「背后」：盒子永远贴在最底层（桌面窗口正上方），它背后就是壁纸。
//
// 两条路：
//   1) 屏幕捕获（默认）—— 能拿到「盒子背后真正的画面」，包括压在桌面上的窗口和
//      动态壁纸。代价是**必须先把我们自己所有的窗口排除掉**（见下面
//      CaptureScreenRegionFast 的实测记录），否则抓回来的就是自己。
//   2) PaintDesktop（兜底）—— 只画壁纸，天生不会抓到自己，但看不见任何窗口。
//      系统不支持 WDA 排除时用它。
#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <atomic>
#include <cstdint>
#include <memory>
#include <vector>

// 把壁纸的屏幕区域 (x,y,w,h) 画进 out（w*h*4 的 BGRA，不透明，自上而下）。
// 失败时 out 为空。
bool CaptureDesktopRegion(int x, int y, int w, int h, std::vector<uint8_t>& out);

// 抓取屏幕 (x,y,w,h) 区域（带 CAPTUREBLT，**会抓到自己**，调用方负责先排除）
bool CaptureScreenRegion(int x, int y, int w, int h, std::vector<uint8_t>& out);

// 同上，但不带 CAPTUREBLT。
//
// 【2026-10 实测，别再信「分层窗口天然不进画面」这句话】
// 在本机（Win11 22621 / 2560×1440）上写了个最小复现程序验证：
//   · 建一个 8×8 的 per-pixel alpha 分层窗口，涂纯红；
//   · 普通 BitBlt（**不带 CAPTUREBLT**）抓它那块：均值 R 从壁纸的 152 变成 164
//     —— 8×8/24×24 的红块正好算出来 163.4，说明**分层窗口照样被抓进来**；
//   · SetWindowDisplayAffinity(WDA_EXCLUDEFROMCAPTURE) 之后再抓：回到 152
//     —— 排除确实生效（**只对调用进程自己的窗口有效**，别的进程设会返回 FALSE）；
//   · 带不带 CAPTUREBLT 的结果逐字节一样（同一块区域 diff = 0）。
// 也就是说 CAPTUREBLT 在这里没有区别，唯一的办法是 WDA 排除。
// 不排除的后果不是「抓到一点点自己」，而是**整块底图变成自己上一帧的画面**：
// 磨砂自我回授，越糊越暗，而且盒子自己的变化再也传不进来 —— 表现就是
// 「亚克力背景永远不对、后面的东西变八百年了它也不变」。
// 所以：任何抓屏都必须包在 ExclGuard 里（见 backdrop.cpp）。
bool CaptureScreenRegionFast(int x, int y, int w, int h, std::vector<uint8_t>& out);

// 抓屏期间把「本进程自己的窗口」从画面里摘掉。
//
// exclude = true：把跟 rect 相交的自家可见窗口全部 SetWindowDisplayAffinity(
// WDA_EXCLUDEFROMCAPTURE)；false：还原。桌面图标层是整屏的分层窗口，永远相交，
// 所以每次都会处理它 —— 不排除它，磨砂里就会糊进一层桌面图标。
//
// 由主程序注册（main.cpp），因为「本进程有哪些窗口」只有它知道。
// 注意：这个钩子会在**工作线程**上被调用（后台抓底图），实现方自己保证线程安全
// （backdrop.cpp 里已经用一把互斥锁把整段「排除→抓屏→还原」串起来了）。
using CaptureExcludeFn = void (*)(const RECT&, bool);
void SetCaptureExcludeHook(CaptureExcludeFn fn);

// 抓屏相关的诊断信息（只报一次，写进日志）。排查「磨砂不对」时先看它。
using CaptureLogFn = void (*)(const char*);
void SetCaptureLogHook(CaptureLogFn fn);

// 三次盒式模糊 ≈ 高斯。px 为 w*h*4 的 BGRA。
void BlurRgba(std::vector<uint8_t>& px, int w, int h, int radius, int passes = 3);

// 快速模糊：先按 4x4 块平均缩到 1/4，在小图上做 radius/4 的模糊，再双线性放大回来。
//
// 大半径盒式模糊的代价和像素数成正比，而模糊本身又会把细节全抹掉 ——
// 所以「先缩小再模糊」在视觉上几乎是同一张图（实测和全分辨率版 RMS 差 1.5/255，
// 肉眼分辨不出），但快了 10 倍以上（32ms → 约 6ms）。
// 这是拖动时不做「每帧重抓」的唯一前提，别改回去。
void BlurFastRgba(std::vector<uint8_t>& px, int w, int h, int radius, int passes = 3);

// 判断一片 BGRA 的颜色是否几乎一致（用来识别「底图无效」）
bool NearlyUniform(const std::vector<uint8_t>& px, int w, int h);

// 一步到位：抓「盒子背后」那一块并模糊。
// 默认走屏幕捕获（内部用 WDA_EXCLUDEFROMCAPTURE 把自家窗口排除掉），因为动态壁纸
// （Wallpaper Engine 之类）只有抓屏才拿得到；preferDesktop = true 时优先 PaintDesktop。
// self = 本次抓取要排除的那个窗口（盒子自己）。系统不支持 WDA 排除时（或者没有
// 注册排除钩子）自动退回「只抓壁纸」的 PaintDesktop 路径 —— 宁可少一层窗口，
// 也绝不能把盒子自己抓进去。
bool CaptureBlurredBackdrop(int x, int y, int w, int h, int blurRadius,
                            std::vector<uint8_t>& out, bool preferDesktop = false,
                            HWND self = nullptr);

// 一步到位（快）：抓到了就用快速模糊。
//
// uniform 非空时会写回「抓到的是不是一片纯色」。纯色有两种可能：
//   · 这块地方本来就是纯色（纯色壁纸 / 空白区域）—— 结果是对的，直接用
//   · 壁纸是别人家的分层窗口画的（某些动态壁纸）—— 那个窗口不归我们管，排除不掉，
//     抓到的确实只有它下面那一层
// 分不清，所以只报告事实，由调用方决定。
bool CaptureBlurredBackdropFast(int x, int y, int w, int h, std::vector<uint8_t>& out,
                                bool* uniform = nullptr, HWND self = nullptr);

// ---------------------------------------------------------------------------
// 共享磨砂帧（2026-10）
//
// 为什么要有它：抓屏那 6ms 是 GPU 回读的**固定**成本，和抓多大面积几乎无关。
// 本机实测（2560×1440）：
//     16×16        6.45 ms
//     444×401      6.42 ms
//     2560×1440   20.65 ms      ← 全屏只比一小块贵 3 倍
//     加一次排除/还原自身   6.30 ms（WDA 免费）
//     每次重建 DIB / GetDC  6.20 ms（也可以忽略）
// 所以「每个盒子各抓一次」等于把同一份固定成本付了 7 遍（≈45ms），这才是
// 「只能两秒刷一次」的真正原因 —— **不是不能实时，是抓的次数太多**。
// 改成「抓一张、所有盒子从里面裁自己那块」之后，固定成本只付一次。
//
// 实现上：一个后台线程按 interval（默认 150ms ≈ 6.5Hz）抓一块区域
// （主程序给的所有可见磨砂盒子的并集）、降采样 + 模糊，结果放进一个带 serial 的
// BackdropFrame。盒子在自己的局部坐标里裁一块带走（纯 memcpy，实测 0.03ms），
// 完全不需要各自 BitBlt。
//
// 注意：裁剪都在**屏幕坐标**上算，scale 只影响像素密度。
// px 存的是「已经模糊过」的图，裁剪后可以直接当底图用。
// ---------------------------------------------------------------------------
struct BackdropFrame {
    int x = 0, y = 0, w = 0, h = 0;   // 覆盖的屏幕坐标范围（≠ 一定是整屏，见上）
    int scale = 2;                     // 像素边长 = 屏幕边长 / scale
    unsigned long long serial = 0;     // 每抓一帧 +1：盒子靠它判断「有没有新帧」
    std::vector<uint8_t> px;           // 1/scale 缩放的模糊图，BGRA

    int pixW() const { return w > 0 ? (w + scale - 1) / scale : 0; }
    int pixH() const { return h > 0 ? (h + scale - 1) / scale : 0; }
};

// 启动帧泵。抓到新帧时会 PostThreadMessage(uiThreadId, wakeMsg, 0, 0)
// 叫醒 UI 线程（和缩略图那条线程用的是同一套办法）。
void InitBackdropFramePump(DWORD uiThreadId, UINT wakeMsg);

// 告诉帧泵「这些区域需要磨砂」（屏幕坐标，一般是所有可见磨砂盒子的并集）。
// 传 nullptr = 现在没人需要，泵彻底停手：不 BitBlt、不占 CPU、也不占那十几 MB
// 整屏 DIB —— 玩游戏 / 全部盒子被盖住 / 没开磨砂的时候就该这样。
//
// 【为什么不是整屏】整屏抓一次要 14.7MB 的 DIB（2560×1440）和 20.65ms，
// 而 BitBlt 的成本基本随面积走（固定 ~6ms + 每 MB 约 1.4ms）。盒子通常只占
// 屏幕一角，按并集抓就既是 3~10ms、又只占 1~4MB。区域变化（挪盒子）会让
// 盒子重新裁一次，那本来就是该重画的时刻。
void SetBackdropFrameRegion(const RECT* region);

// 拖动 / 改尺寸期间暂停抓屏。
//
// 【为什么拖的时候要停】BitBlt 要抢 win32k 的全局 GDI 锁，而 UI 线程每帧的
// SetWindowPos / UpdateLayeredWindow 也要那把锁；拖拽是唯一「每帧都要跟手」的时刻，
// 这段时间宁可让背景冻住（反正可以从已经抓好的一帧里重新裁），也绝不抓屏。
void SetBackdropFramePaused(bool paused);

// 抓一帧的间隔（毫秒），默认 150。环境变量 MEOW_BD_MS 可覆盖（调试用）。
void SetBackdropFrameInterval(DWORD ms);

// 当前最新的一帧。可能为空（还没抓过 / 没开），也可能是旧的 —— 调用方自己看 serial。
std::shared_ptr<const BackdropFrame> CurrentBackdropFrame();

// 从帧里裁一块（want 是屏幕坐标），输出 1/scale 缩放的像素，covered 写回真正覆盖的范围。
// 覆盖范围一定是 scale 对齐的，所以「屏幕宽 / scale」和像素宽严格相等。
bool CropBackdropFrame(const BackdropFrame& f, const RECT& want, std::vector<uint8_t>& out,
                       RECT& covered);

// ---------------------------------------------------------------------------
// 后台抓取（拖动时不卡的关键）
//
// 一次「抓屏 + 模糊」实测 18ms 上下（抓屏 8~11ms 是 GPU 回读，压不下去；
// 模糊已经从 35ms 压到 7ms）。同步做完必然吃掉一帧，而拖动时盒子每挪 128px
// 就要补抓一次 —— 于是就是「一卡一卡」。
//
// 所以把这一步整个搬到工作线程：UI 线程只提交矩形，然后每帧问一句「好了没」，
// 好了就换上新的底图。工作线程里只做 BitBlt + 纯内存运算，不碰任何窗口
// （WDA 亲和性只能在窗口所属线程改），所以不存在跨线程窗口操作。
// ---------------------------------------------------------------------------
struct BackdropCaptureJob {
    RECT rect{};
    std::vector<uint8_t> px;
    int x = 0, y = 0, w = 0, h = 0;
    bool ok = false;              // 抓到了画面（BitBlt 成功）
    bool uniform = false;         // 但整块是纯色（见 CaptureBlurredBackdropFast 注释）
    std::atomic<bool> finished{false};
};

// 提交一次后台抓取；返回的 job 由调用方持有（shared_ptr，线程安全）。
// 队列里积压太多（>4）会直接丢掉本次请求，返回 nullptr —— 拖动很快时没必要
// 补抓那么多陈旧位置，等下一帧自然会重新提交最近的矩形。
// self：要排除的自家窗口（工作线程自己调 WDA，不需要 UI 线程配合）。
std::shared_ptr<BackdropCaptureJob> BackdropCaptureAsync(const RECT& rect,
                                                         HWND self = nullptr);

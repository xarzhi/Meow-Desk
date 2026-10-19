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
// 用 PaintDesktop 把壁纸按盒子所在的屏幕区域画到内存 DC 里，再模糊。
// 这样不依赖屏幕捕获，也就不存在「抓到自己上一帧」的回授问题。
//
// 兜底：PaintDesktop 在部分环境（动态壁纸、某些第三方 shell）会失败或画出一片纯色，
// 那时退回 BitBlt 抓屏（配合 WDA_EXCLUDEFROMCAPTURE 把自己排除掉）。
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

// 抓取屏幕 (x,y,w,h) 区域（会抓到自己，调用方负责先把窗口排除掉）
bool CaptureScreenRegion(int x, int y, int w, int h, std::vector<uint8_t>& out);

// 采样屏幕 (x,y,w,h) 区域的平均颜色，写进 r/g/b（0-255）。
//
// 用途只有一个：当「壁纸指纹」。整块缩到 16×16 再求均值，一次 StretchBlt
// 是微秒级，可以每秒问一圈而不心疼；换了壁纸均值就会变，没换就一笔不动。
//
// 和 CaptureScreenRegionFast 一样**不带 CAPTUREBLT** —— 带上它会把我们自己的
// 分层窗口（桌面图标层 / 盒子）也抓进来，那样量到的就是我们自己上一帧的画面。
bool SampleScreenMean(int x, int y, int w, int h, int& r, int& g, int& b);

// 同上，但不带 CAPTUREBLT：**分层窗口不进画面**。
//
// 这一条是给「拖动时背景不卡」用的。带 CAPTUREBLT 时，我们自己的盒子（都是分层窗口）
// 会被抓进去，于是每次抓屏都得先 SetWindowDisplayAffinity(WDA_EXCLUDEFROMCAPTURE)
// 把自己和别的自家窗口排除掉，抓完再改回来 —— 每一次改动都要求 DWM 重新合成一遍
// 桌面，而且是慢路径（EnumWindows + 每个窗口一次调用）。拖动时每 100ms 来这么一轮，
// 看到的就正好是「一卡一卡」。不带 CAPTUREBLT 之后分层窗口天然被跳过，
// 一次 GDI 调用就干净了，实测速度一样（7~8ms），但没有任何副作用。
bool CaptureScreenRegionFast(int x, int y, int w, int h, std::vector<uint8_t>& out);

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
// 默认走屏幕捕获（配合 WDA_EXCLUDEFROMCAPTURE 把自己排除），因为动态壁纸
// （Wallpaper Engine 之类）只有抓屏才拿得到；preferDesktop = true 时优先 PaintDesktop。
bool CaptureBlurredBackdrop(int x, int y, int w, int h, int blurRadius,
                            std::vector<uint8_t>& out, bool preferDesktop = false);

// 一步到位（快）：不走 CAPTUREBLT、不碰任何窗口的 WDA 亲和性，抓到了就用快速模糊。
//
// uniform 非空时会写回「抓到的是不是一片纯色」。纯色有两种可能：
//   · 壁纸本身就是纯色 —— 结果是对的，直接用
//   · 壁纸是分层窗口画的（某些动态壁纸）—— 这条路看不见它，得退回去走慢路径
// 分不清，所以只报告事实，由调用方决定（见 FenceWindow：先确认一次，之后就不折腾了）。
bool CaptureBlurredBackdropFast(int x, int y, int w, int h, std::vector<uint8_t>& out,
                                bool* uniform = nullptr);

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
std::shared_ptr<BackdropCaptureJob> BackdropCaptureAsync(const RECT& rect);

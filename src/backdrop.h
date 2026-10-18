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

#include <cstdint>
#include <vector>

// 把壁纸的屏幕区域 (x,y,w,h) 画进 out（w*h*4 的 BGRA，不透明，自上而下）。
// 失败时 out 为空。
bool CaptureDesktopRegion(int x, int y, int w, int h, std::vector<uint8_t>& out);

// 抓取屏幕 (x,y,w,h) 区域（会抓到自己，调用方负责先把窗口排除掉）
bool CaptureScreenRegion(int x, int y, int w, int h, std::vector<uint8_t>& out);

// 三次盒式模糊 ≈ 高斯。px 为 w*h*4 的 BGRA。
void BlurRgba(std::vector<uint8_t>& px, int w, int h, int radius, int passes = 3);

// 判断一片 BGRA 的颜色是否几乎一致（用来识别「底图无效」）
bool NearlyUniform(const std::vector<uint8_t>& px, int w, int h);

// 一步到位：抓「盒子背后」那一块并模糊。
// 默认走屏幕捕获（配合 WDA_EXCLUDEFROMCAPTURE 把自己排除），因为动态壁纸
// （Wallpaper Engine 之类）只有抓屏才拿得到；preferDesktop = true 时优先 PaintDesktop。
bool CaptureBlurredBackdrop(int x, int y, int w, int h, int blurRadius,
                            std::vector<uint8_t>& out, bool preferDesktop = false);

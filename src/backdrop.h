// 亚克力底图：抓取窗口背后的屏幕区域并做高斯近似模糊
//
// 用 SetWindowDisplayAffinity(WDA_EXCLUDEFROMCAPTURE) 把自己的窗口排除在截屏之外，
// 这样 BitBlt 屏幕时抓到的就是"盒子背后"的内容，不会把自己也截进去。
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

// 抓取屏幕 (x,y,w,h) 区域并模糊。out 为 w*h*4 的 BGRA（不透明），自上而下。
bool CaptureBlurredBackdrop(int x, int y, int w, int h, int blurRadius,
                            std::vector<uint8_t>& out);

// 多屏几何：统一的「这块矩形属于哪块屏、能挪到哪」判断
//
// 以前各个窗口各写一遍，而且兜底一律是
//   RECT lim{0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN)};
// 也就是「主屏尺寸」。单屏没问题；多屏时只要选错显示器（比如按矩形左上角去
// MonitorFromPoint，而左上角还停在旧屏上），盒子就会被旧屏的工作区夹住 ——
// 表现就是「拖到两块屏交界处就再也过不去了」。
//
// 这里统一按「矩形跟哪块屏重叠的面积最大」选屏；跨屏时把相交那几块屏的
// 工作区并起来（取包围盒），交界处就不会被两套工作区轮流夹。
#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

namespace ScreenUtil {

// 整个虚拟桌面（所有显示器拼起来的外框）
RECT VirtualScreen();

// 某个点所在显示器的工作区；取不到时退回 Windows 的默认工作区
RECT WorkAreaForPoint(POINT pt);

// 矩形所在显示器的工作区。跨屏时返回「所有相交屏工作区的包围盒」。
RECT WorkAreaForRect(const RECT& r);

// 把一个矩形夹进给定工作区（宽高不变；工作区比矩形还小时以左上角为准）
RECT ClampRect(const RECT& r, const RECT& wa);

// 光标所在的显示器工作区（弹窗居中 / 新建窗口用）
RECT WorkAreaForCursor();

}  // namespace ScreenUtil

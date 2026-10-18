// 窗口层级：让我们的窗口"紧贴桌面窗口正上方"，而不是置顶。
//
// 抄的是 my-desktop 的 DesktopHost.KeepAboveDesktop 机制：
//   · 平时桌面窗口在最底层，所以我们的窗口自然位于所有应用窗口之下
//   · Win+D「显示桌面」把桌面提到最前时，我们的窗口跟着上移，依然可见
//   · 做法是在 WM_WINDOWPOSCHANGING 里把 hwndInsertAfter 改写成"桌面上方紧邻的窗口"，
//     任何层级变化（被点击激活、Show、系统重排）都会自动被纠正回原位
//
// 为什么不 SetParent 到桌面窗口：跨进程的父子/所有者关系会让本程序与 Explorer
// 共享输入队列，右键菜单这类模态交互会互相等待而死锁（my-desktop 注释里明确警告过）。
#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <functional>

// 桌面图标视图（SHELLDLL_DefView）所在的顶层窗口。
// 通常是 Progman；用过动态壁纸 / 换壁纸淡入动画后可能被挪到某个 WorkerW 里。
HWND FindDesktopWindow();

// 让 hwnd 始终紧贴 resolveAnchor() 返回的窗口正上方。
// 用 SetWindowLongPtr 子类化窗口过程，anchorFn 在每次层级变化时重新求值（桌面句柄会变）。
void InstallKeepAbove(HWND hwnd, std::function<HWND()> anchorFn);
void RemoveKeepAbove(HWND hwnd);

// 立即把 hwnd 摆到 anchor 正上方
void PlaceAbove(HWND hwnd, HWND anchor);

// hwnd 是否已经紧贴在 anchor 正上方（中间没有别的可见窗口）
bool IsPinnedAbove(HWND hwnd, HWND anchor);

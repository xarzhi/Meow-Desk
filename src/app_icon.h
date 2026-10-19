// app 图标 / 关于页 logo 的统一入口
//
// 图标本体（res/meow-desk.ico）由 RC 编进 exe，所以这里只做「按尺寸取一个 HICON」。
// 托盘图标、设置窗口、搜索窗口都走这里，换图标只改资源文件一处。
#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

namespace Gdiplus {
class Bitmap;
}

// 取 exe 里某个 RCDATA 的原始字节（进程生命周期内有效，**不要 free**）。
// 找不到返回 nullptr、size = 0。图标字体也走这里。
const void* AppResourceBytes(int id, DWORD& size);

// 从 exe 资源里取 app 图标。size 是逻辑像素（托盘 32、小图标 16）。
// 取不到返回 nullptr —— 调用方必须自己兜底（旧的渐变方块），
// 否则资源没编进去时托盘会是个空白方块。
HICON LoadAppIcon(int size);

// 关于页要画的 logo（128px PNG）。首次调用时解码，之后返回同一个对象。
// 失败返回 nullptr（关于页就只显示文字）。**不要 delete**。
Gdiplus::Bitmap* AppLogoBitmap();

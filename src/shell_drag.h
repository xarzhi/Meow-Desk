// 把文件「拖出我们自己的程序」：交给系统去跑一次真正的 OLE 拖放
//
// 我们内部拖拽完全是自绘的（拖拽浮层 + 落点提示 + 盒子吸附），从头到尾只有
// SetCapture，没有向系统提供任何数据。于是拖到浏览器上传区、别的程序窗口上时，
// 对方根本不知道有东西在拖 —— 松手当然什么都没发生。
//
// 要让它「真的在拖文件」，唯一的路是实现 IDataObject（CF_HDROP）+
// IDropSource，交给 DoDragDrop 跑系统的拖放循环：目标窗口会去 QueryGetData
// 拿 CF_HDROP，浏览器据此构造 File 对象、资源管理器据此搬文件。
#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
// DROPEFFECT_* 在 objidl.h 里；WIN32_LEAN_AND_MEAN 会把 ole2.h 从 windows.h 里剔掉，
// 所以下面这个结构体光靠 windows.h 编不过（error C2065: DROPEFFECT_NONE 未声明）。
#include <objbase.h>
#include <ole2.h>

#include <functional>
#include <string>
#include <vector>

namespace ShellDrag {

// 一次系统拖放的结果。
struct DragOutcome {
    DWORD effect = DROPEFFECT_NONE;
    // 用户按 Esc / 右键取消
    bool cancelled = false;
    // 光标离开了「别人的窗口」→ 我们主动让拖放循环返回 CANCEL（见 keepDragging）。
    // 这时鼠标左键**通常还按着**，调用方可以把内部拖拽接回去，用户感觉是
    // 「拖过别人窗口又回到桌面，图标重新跟手」。
    bool leftTarget = false;
};

// 开始一次系统级文件拖放。**阻塞**：从调用到用户松手/按 Esc 期间由 OLE 的消息
// 循环接管。
//
// keepDragging：每次 QueryContinueDrag（约等于每帧）问一次「这个屏幕点还算在
// 拖放目标里吗」。返回 false 就当作「用户把文件拖回自己家窗口了」，立刻取消这次
// 系统拖放并回 DragOutcome::leftTarget —— 不这么做的话，一旦交棒给 OLE，我们就
// 只能等用户松手，中途想换回内部拖拽（换个桌面位置）没有任何办法。
// 不传 = 一直拖到松手。
//
// paths 必须是磁盘上真实存在的路径。虚拟项（回收站 / 此电脑，路径形如 ::{GUID}）
// 没法用 CF_HDROP 表达，调用方要先过滤掉；全是虚拟项时别调这个函数。
DragOutcome DragFiles(const std::vector<std::wstring>& paths,
                      std::function<bool(POINT)> keepDragging = {});

}  // namespace ShellDrag

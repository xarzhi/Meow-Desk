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

#include <string>
#include <vector>

namespace ShellDrag {

// 开始一次系统级文件拖放。**阻塞**：从调用到用户松手/按 Esc 期间由 OLE 的消息
// 循环接管。返回最终的 DROPEFFECT_*（DROPEFFECT_NONE = 取消或没人接收）。
//
// paths 必须是磁盘上真实存在的路径。虚拟项（回收站 / 此电脑，路径形如 ::{GUID}）
// 没法用 CF_HDROP 表达，调用方要先过滤掉；全是虚拟项时别调这个函数。
DWORD DragFiles(const std::vector<std::wstring>& paths);

}  // namespace ShellDrag

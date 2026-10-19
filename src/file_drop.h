#pragma once
#include <windows.h>

#include <functional>
#include <string>
#include <vector>

// 让我们的窗口接受「从资源管理器 / 浏览器 / 任何支持 CF_HDROP 的程序」拖过来的文件。
//
// 窗口不注册 IDropTarget 的话，Windows 按「这里不接受拖放」处理：光标直接变成
// 禁止符号（一个圈加斜杠），松手什么都不发生。桌面层和盒子都是普通 HWND，
// 光靠 WM_DROPFILES（DragAcceptFiles）不行 —— 那个只对同一次拖放里的老式
// 消息有效，现代拖放（浏览器、Explorer 的 CF_HDROP 传输）走的是 OLE，
// 必须实现 IDropTarget，否则只能看着光标变禁止。
namespace FileDrop {

// 在「创建窗口的那个 UI 线程」上调用一次（内部会做 OLE 初始化）。
// 需要在 WinMain 里创建任何窗口之前调。
void Init();

// 给 hwnd 挂上拖放接收。
// onDrop 参数：文件路径列表 + 松手时的**屏幕坐标**（调用方自己转客户端）。
// 返回值必须是「我们真正做了什么」：DROPEFFECT_MOVE / DROPEFFECT_COPY / DROPEFFECT_NONE。
// 这个值会回给拖放源 —— 报成 MOVE 源程序就会把原文件删掉，所以只做了复制时千万不能报 MOVE。
void Attach(HWND hwnd,
            std::function<DWORD(const std::vector<std::wstring>&, POINT)> onDrop);

// 窗口销毁前摘下（RevokeDragDrop）。不摘会让 OLE 一直挂着一个死窗口。
void Detach(HWND hwnd);

}  // namespace FileDrop

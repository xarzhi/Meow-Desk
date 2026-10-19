// 行内重命名输入框
//
// 为什么自己做：系统右键菜单里的「重命名」动词背后要求宿主是一个真正的
// IShellView（资源管理器靠它开原地编辑框）。我们的桌面是我们自己画的窗口，
// 不是 shell 视图 —— 那个动词点下去什么都不会发生。所以菜单项由 shell 提供，
// 点下去之后由我们自己弹这个输入框。
#pragma once

#include <windows.h>

#include <functional>
#include <string>

namespace RenameBox {

// 在屏幕矩形 labelRect 处弹出一个命名框，预填 currentName，并**只选中主文件名**
// （不含扩展名，和资源管理器一致：直接打字不会把 .lnk/.txt 打掉）。
// 回车 / 失焦 → 提交；Esc → 取消。提交回调拿到的是用户输入的完整文件名。
void Show(HWND owner, RECT labelRect, const std::wstring& currentName,
          std::function<void(const std::wstring&)> onCommit);

// 关掉（取消）。换分组、重新枚举图标之前调一下，避免回调里还在用旧的图标指针。
void Close();

bool IsOpen();

}  // namespace RenameBox

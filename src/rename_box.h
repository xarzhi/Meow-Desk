// 重命名弹窗（自绘卡片 + 原生输入框）
//
// 演进：最早是「贴着标题原地弹一个小输入框」，两个实际问题：
//   1. 位置跟着图标/标签跑 —— 贴着盒子右边缘的图标，输入框会被挤出屏幕或压住
//      旁边一排图标；盒子被收起/滚动时锚点还算不准。
//   2. 盒子标题/分组标签的改名是另一套（主窗口里另建一个 EDIT），两套行为不一致
//      （一个失焦提交、一个失焦取消），用户点到别处得到的后果不一样。
// 现在统一成一个居中的小卡片：锚点传盒子窗口 → 摆在盒子正中；锚点为空 → 屏幕正中。
//
// 为什么卡片要自己画：系统右键菜单里的「重命名」动词背后要求宿主是真正的
// IShellView（资源管理器靠它开原地编辑框），我们的桌面是自绘窗口，不是 shell
// 视图 —— 那个动词点下去什么都不会发生。所以菜单项交给 shell，改名的 UI 归我们。
#pragma once

#include <windows.h>

#include <functional>
#include <string>

namespace RenameBox {

// 预选范围（决定输入框里一开始选中哪一段）
enum class Sel {
    All,    // 全选 —— 改盒子名 / 分组名
    Stem,   // 只选主文件名，不含扩展名 —— 改文件名，直接打字不会把 .lnk/.txt 打掉
};

// anchor：卡片摆在它正中（传盒子窗口）。nullptr = 摆在当前屏幕正中。
// title：卡片标题，比如「重命名」「重命名分区」「重命名分组」。
// 回车 / 点「确定」→ 提交；Esc / 点「取消」/ 焦点跑到别的程序 → 放弃。
void Show(HWND anchor, const std::wstring& title, const std::wstring& currentName,
          std::function<void(const std::wstring&)> onCommit, Sel sel = Sel::All);

// 关掉（按取消处理）。换分组 / 重新枚举图标之前调一下，
// 避免回调里还在用已经失效的图标指针。
void Close();

bool IsOpen();

}  // namespace RenameBox

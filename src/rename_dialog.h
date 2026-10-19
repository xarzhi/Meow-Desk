// 重命名弹窗（自绘卡片 + 原生输入框 + 确定/取消）
//
// 用途：改**分区（盒子）标题**和**分组名**。用户要的就是这个弹窗 —— 他说过
// 「右键重命名或者双击 tab，直接在相应的盒子内打开一个弹窗修改名称」。
// 图标改名不走这里，走 rename_box.h 的 ShowInPlace（资源管理器那种就地编辑）。
//
// 为什么卡片要自己画：系统右键菜单里的「重命名」动词背后要求宿主是真正的
// IShellView（资源管理器靠它开原地编辑框），我们的桌面是自绘窗口，不是 shell
// 视图 —— 那个动词点下去什么都不会发生。所以菜单项交给 shell，改名的 UI 归我们。
//
// 演进（别再反复）：
//   · 最早：贴着标题原地弹输入框 → 贴盒子右边缘的图标会被挤出屏幕，滚动的盒子锚点算不准
//   · c903631 / d3d55a3：改成盒子内居中的弹窗（这一版，输入框文字垂直居中）
//   · 937e16d：一度整体改回就地编辑（那是给**图标**改名用的交互，被误用到盒子/分组上）
//   · 现在：两套并存 —— 图标就地编辑（rename_box），盒子/分组用弹窗（本文件）
#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <functional>
#include <string>

namespace RenameDialog {

// 预选范围（决定输入框里一开始选中哪一段）
enum class Sel {
    All,    // 全选 —— 改盒子名 / 分组名
    Stem,   // 只选主文件名，不含扩展名
};

// 弹一个居中的改名卡片。
//
// anchor：卡片摆在它正中（传盒子窗口）。nullptr = 摆在鼠标所在屏幕正中。
// title ：卡片标题，比如「重命名分区」「重命名分组」。
// sel   ：输入框初始选中范围。
// onDone：**提交或取消都会调一次**，在 onCommit 之后。主程序用它解除
//         「改名期间别自动收起盒子」的占位。
//
// 回车 / 点「确定」→ 提交；Esc / 点「取消」/ 焦点跑到别的程序 → 放弃。
void Show(HWND anchor, const std::wstring& title, const std::wstring& currentName,
          std::function<void(const std::wstring&)> onCommit, Sel sel = Sel::All,
          std::function<void()> onDone = nullptr);

// 关掉（按取消处理）。换分组 / 重新枚举图标之前调一下，
// 避免回调里还在用已经失效的指针。
void Close();

bool IsOpen();

}  // namespace RenameDialog

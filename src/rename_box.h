// 重命名：贴在「原来那个名字」上的就地输入框
//
// 演进：最早的实现就是原地输入框，但当时有两个实际问题（贴着盒子右边缘的图标会被挤出
// 屏幕、盒子滚动后锚点算不准），于是第 25 批改成「盒子内居中的卡片」（带标题和
// 确定/取消按钮）。用户实测后要的还是**资源管理器那种就地改名**：不弹窗、不挪位置，
// 直接在图标标题那一行上写字。所以现在只留输入格：没有标题、没有按钮。
//
// 交互和资源管理器一致：
//   · 回车 → 提交；Esc → 放弃
//   · 焦点跑到别的程序（点了别处）→ **提交**（不是放弃）
//
// 为什么还是要一对自绘分层窗口 + owned EDIT：
//   · per-pixel alpha 的分层窗口才能做出圆角 + 柔和阴影，和菜单/盒子同一套观感；
//   · 分层窗口不能放子控件（子窗口不参与 ULW 合成）→ 输入框做成它拥有的顶层 EDIT，
//     光标闪烁、拖选、输入法全是系统原生的；
//   · 桌面层和盒子都是 WS_EX_NOACTIVATE，只有这个能激活的顶层窗口收得到键盘。
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

namespace RenameBox {

// 预选范围（决定输入框里一开始选中哪一段）
enum class Sel {
    All,    // 全选 —— 改盒子名 / 分组名
    Stem,   // 只选主文件名，不含扩展名 —— 改文件名，直接打字不会把 .lnk/.txt 打掉
};

// 贴着一个屏幕矩形就地编辑。
//
// rect 是「被改的名字现在占的那块区域」：图标标题的文字盒 / 分组标签 / 盒子标题行。
// 输入框会水平居中于它、上沿和第一行文字对齐、宽度至少盖住它，高度只取一行
// （rect 高度够两行时取两行，免得选中态展开的第二行从输入框下面露出来）。
//
// onDone 在**无论提交还是取消**都会调一次（在 onCommit 之后），
// 给调用方做收尾用 —— 比如改名期间占住盒子的「悬停自动收起」。
void ShowInPlace(const RECT& rect, const std::wstring& currentName,
                 std::function<void(const std::wstring&)> onCommit, Sel sel = Sel::All,
                 std::function<void()> onDone = nullptr);

// 关掉（按取消处理）。换分组 / 重新枚举图标之前调一下，
// 避免回调里还在用已经失效的图标指针。
void Close();

// 提交（等同于回车）。点到别处时用这个收尾 —— 资源管理器也是提交，不是放弃。
void Commit();

bool IsOpen();

}  // namespace RenameBox

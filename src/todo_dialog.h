// 待办弹窗：新建待办 / 查看详情
//
// 用户要求（原话）：
//   「把待办盒子下面的新建待办去掉，在待办盒子头部右边添加一个 + 按钮，点击弹出一个
//     新建待办的窗口，在屏幕中央，输入好待办内容后点击确定再在待办盒子内添加这个待办，
//     然后待办列表中的待办项右边添加一个查看详情按钮，点击后打开弹窗查看这个待办的详情，
//     并且可以在弹窗内直接点击完成代办按钮」
//
// 所以这里两个模式共用同一张骨架（和 rename_dialog / confirm_dialog 同一套配方）：
//   · Input  ：标题 + 空文本域 + 取消/确定
//   · Detail ：标题 + 状态徽标 + 正文文本域 + 关闭/完成（正文下面一行显示创建时间）
//     —— 未完成的详情**正文可以直接改**，已完成的详情只读（用户要求）。
//
// 两张卡片都能**拖动**，但只有头部（标题那一行）能拖，而且不改鼠标样式（用户要求）。
//
// 为什么正文用 GDI+ DrawString 而盒子里用 GDI 栅格化：
// 卡片底色是**不透明**的（这里没有 per-pixel 半透明底），不存在「笔画摊到壁纸上发灰」
// 那个问题；而换行排版（中文按字断行、末行省略号）只有 GDI+ 那套排版器现成。
// 注意：文字现在由 EDIT 控件自己画，GDI+ 只剩标题 / 徽标 / 按钮 / 提示这几处。
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

namespace TodoDialog {

// 新建待办。owner 非空就居中于 owner 所在的那块屏（待办盒），否则居中于鼠标所在屏。
// Ctrl+Enter / 点「确定」→ onOk(去掉首尾空白的文字，换行统一成 LF)；
// Esc / 点「取消」/ 焦点跑掉 → 放弃。单独一个 Enter 是换行（文本域）。
// onDone：**两条路都会调一次**（在 onOk 之后），用来解除「弹窗期间别自动收盒子」。
void ShowInput(HWND owner, const std::wstring& initial,
               std::function<void(const std::wstring&)> onOk,
               std::function<void()> onDone = nullptr);

// 查看详情：正文自动换行（未完成可直接改，已完成只读），底部「关闭 / 完成（恢复未完成）」。
// created 是创建时间（Unix 秒，0 = 不知道），显示在正文下面那一行。
// onSave：**未完成且内容真改过**才回调（关窗时统一触发；清空当作没改，不保存）。
// onToggleDone：点了那个主按钮才回调（已完成状态下列面写「恢复」）。
void ShowDetail(HWND owner, const std::wstring& text, bool done, long long created,
                std::function<void(const std::wstring&)> onSave,
                std::function<void()> onToggleDone,
                std::function<void()> onDone = nullptr);

// 按「取消」关掉。换 tab / 删盒子 / 重新排版之前调一下，避免回调里用到失效的数据。
void Close();
bool IsOpen();

}  // namespace TodoDialog

// 「用系统默认方式打开一个路径」的唯一入口。
//
// 为什么需要这么一层：**ShellExecute 对「没有扩展名的路径」会按 PATHEXT 补全**。
// 它先判断这个字符串能不能当成文档打开，不行就退回命令行语义（CreateProcess 那套），
// 于是把 `...\show-your-key` 补成 `...\show-your-key.exe` 直接跑起来 ——
// 哪怕同名目录确实存在（实测带结尾反斜杠都挡不住，SEE_MASK_INVOKEIDLIST 也挡不住）。
// 症状：双击文件夹，打开的是隔壁那个同名 exe；「打开数据目录」打开的是主程序自己。
//
// 所以这里按类型分开处理：
//   · 目录            → 交给 explorer.exe，路径当参数传，不经过扩展名补全
//   · 无扩展名的文件  → 走「打开方式」，和资源管理器双击它的行为一致
//   · 其余（含 .lnk / ::{CLSID} 虚拟项）→ 原样交给 ShellExecute，它们自带扩展名或走命名空间
#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <string>

namespace shell_open {

// 返回值沿用 ShellExecute 的口径：> 32 成功，<= 32 是 SE_ERR_* 失败码（0 = 空路径）
INT_PTR Open(const std::wstring& path);

}  // namespace shell_open

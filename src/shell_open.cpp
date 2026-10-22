#include "shell_open.h"

#include <shellapi.h>  // ShellExecuteW
#include <shlobj.h>    // SHOpenWithDialog / OPENASINFO
#include <shlwapi.h>   // PathFindExtensionW

namespace {

bool IsDirectory(const std::wstring& p) {
    const DWORD a = GetFileAttributesW(p.c_str());
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}

bool Exists(const std::wstring& p) {
    return GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES;
}

// 目录：让 explorer.exe 去开，路径作为参数传进去。
// 这条路径不经过 ShellExecute 的「文档解析 → 命令行解析」回退，所以不会去补 .exe。
INT_PTR OpenDirectory(const std::wstring& path) {
    std::wstring params = L"\"" + path + L"\"";
    INT_PTR rc = reinterpret_cast<INT_PTR>(ShellExecuteW(
        nullptr, L"open", L"explorer.exe", params.c_str(), nullptr, SW_SHOWNORMAL));
    if (rc > 32) return rc;
    // explorer 被策略挡掉之类的情况：退回系统默认动词。
    // 这里明知有扩展名补全的坑，但「开不开」已经比「开错」次要 —— 至少先给它一次机会。
    return reinterpret_cast<INT_PTR>(
        ShellExecuteW(nullptr, L"open", path.c_str(), nullptr, nullptr, SW_SHOWNORMAL));
}

// 没有扩展名的普通文件：资源管理器双击它弹的是「打开方式」，
// 而 ShellExecute 会先把它补成 <同名>.exe 跑掉。照抄资源管理器的做法。
INT_PTR OpenWithDialog(const std::wstring& path) {
    OPENASINFO oi{};
    oi.oaifInFlags = OAIF_ALLOW_REGISTRATION | OAIF_EXEC;
    oi.pcszFile = path.c_str();
    const HRESULT hr = SHOpenWithDialog(nullptr, &oi);
    if (SUCCEEDED(hr)) return 42;   // 和 ShellExecute 的「成功」口径对齐（> 32）
    return reinterpret_cast<INT_PTR>(ShellExecuteW(nullptr, L"open", path.c_str(),
                                                  nullptr, nullptr, SW_SHOWNORMAL));
}

}  // namespace

namespace shell_open {

INT_PTR Open(const std::wstring& path) {
    if (path.empty()) return 0;

    if (IsDirectory(path)) return OpenDirectory(path);

    const wchar_t* ext = PathFindExtensionW(path.c_str());
    if ((!ext || !*ext) && Exists(path)) return OpenWithDialog(path);

    return reinterpret_cast<INT_PTR>(
        ShellExecuteW(nullptr, L"open", path.c_str(), nullptr, nullptr, SW_SHOWNORMAL));
}

}  // namespace shell_open

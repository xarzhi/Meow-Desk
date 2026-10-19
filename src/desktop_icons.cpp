// 桌面图标枚举 / 位置写回：跨进程操作资源管理器的桌面视图
// 机制（对齐 my-desktop）：IShellWindows -> IShellBrowser -> IFolderView2，
// 拿 PIDL / 显示名 / 屏幕位置，并用 SelectAndPositionItems 写回新位置。
#include "desktop_icons.h"

#include <shlwapi.h>
#include <objbase.h>
#include <shobjidl.h>
#include <exdisp.h>

#include <cstdio>
#include <cstring>
#include <algorithm>
#include <unordered_set>

#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "shell32.lib")

namespace {

char g_diag[512] = "not run";
POINT g_spacing{75, 98};

void SetDiag(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf_s(g_diag, sizeof(g_diag), _TRUNCATE, fmt, ap);
    va_end(ap);
}

// 从 PIDL 获取解析名（完整路径或 ::{CLSID}）
std::wstring GetNameForParsing(IShellFolder* folder, PCUITEMID_CHILD pidl) {
    STRRET strret{};
    if (FAILED(folder->GetDisplayNameOf(pidl, SHGDN_FORPARSING, &strret)))
        return {};
    wchar_t buf[MAX_PATH]{};
    if (SUCCEEDED(StrRetToBufW(&strret, pidl, buf, MAX_PATH)))
        return buf;
    return {};
}

std::wstring GetDisplayName(IShellFolder* folder, PCUITEMID_CHILD pidl) {
    STRRET strret{};
    if (FAILED(folder->GetDisplayNameOf(pidl, SHGDN_NORMAL, &strret)))
        return {};
    wchar_t buf[MAX_PATH]{};
    if (SUCCEEDED(StrRetToBufW(&strret, pidl, buf, MAX_PATH)))
        return buf;
    return {};
}

bool IsDesktopFolderPath(const std::wstring& key) {
    wchar_t desktop[MAX_PATH]{};
    wchar_t common[MAX_PATH]{};
    if (SUCCEEDED(SHGetFolderPathW(nullptr, CSIDL_DESKTOP, nullptr, 0, desktop)))
        desktop[MAX_PATH - 1] = 0;
    if (SUCCEEDED(SHGetFolderPathW(nullptr, CSIDL_COMMON_DESKTOPDIRECTORY, nullptr, 0, common)))
        common[MAX_PATH - 1] = 0;

    wchar_t full[MAX_PATH]{};
    wchar_t* filepart = nullptr;
    if (!GetFullPathNameW(key.c_str(), MAX_PATH, full, &filepart))
        return false;
    if (filepart) *(filepart - 1) = 0;  // 去掉文件名，只留父目录

    std::wstring parent = full;
    return _wcsicmp(parent.c_str(), desktop) == 0 || _wcsicmp(parent.c_str(), common) == 0;
}

// 找到资源管理器的桌面视图（IFolderView2）。失败返回 nullptr，原因写进 g_diag。
IFolderView2* GetDesktopView() {
    IShellWindows* shellWindows = nullptr;
    HRESULT hr = CoCreateInstance(CLSID_ShellWindows, nullptr,
            CLSCTX_LOCAL_SERVER, IID_IShellWindows, (void**)&shellWindows);
    if (FAILED(hr) || !shellWindows) {
        SetDiag("CoCreateInstance ShellWindows hr=0x%08X", hr);
        return nullptr;
    }

    VARIANT vLoc{};
    vLoc.vt = VT_I4;
    vLoc.lVal = CSIDL_DESKTOP;
    VARIANT vRoot{};
    vRoot.vt = VT_EMPTY;
    IDispatch* disp = nullptr;
    long windowHash = 0;  // FindWindowSW 的 plHash 不接受 NULL（0x800706F4）
    hr = shellWindows->FindWindowSW(&vLoc, &vRoot, SWC_DESKTOP,
            &windowHash, SWFO_NEEDDISPATCH, &disp);
    if (FAILED(hr) || !disp) {
        SetDiag("FindWindowSW hr=0x%08X", hr);
        shellWindows->Release();
        return nullptr;
    }

    IFolderView2* view = nullptr;
    IServiceProvider* sp = nullptr;
    hr = disp->QueryInterface(IID_IServiceProvider, (void**)&sp);
    if (SUCCEEDED(hr) && sp) {
        IShellBrowser* browser = nullptr;
        hr = sp->QueryService(SID_STopLevelBrowser, IID_IShellBrowser, (void**)&browser);
        if (SUCCEEDED(hr) && browser) {
            IShellView* shellView = nullptr;
            if (SUCCEEDED(browser->QueryActiveShellView(&shellView)) && shellView) {
                shellView->QueryInterface(IID_IFolderView2, (void**)&view);
                shellView->Release();
            }
            browser->Release();
        } else {
            SetDiag("QueryService browser hr=0x%08X", hr);
        }
        sp->Release();
    } else {
        SetDiag("QI ServiceProvider hr=0x%08X", hr);
    }
    disp->Release();
    shellWindows->Release();

    if (!view) SetDiag("no IFolderView2");
    return view;
}

}  // namespace

const char* LastEnumDiag() { return g_diag; }

POINT LastEnumSpacing() { return g_spacing; }

// ---------------------------------------------------------------------------
// 和真实文件夹对账
//
// 桌面视图被我们隐藏之后，它对文件系统变化的同步是不可靠的：卸载掉的程序、
// 从浏览器下载的文件，视图里的列表都可能还是旧的。所以枚举完视图之后，
// 再用「文件夹里到底有什么」校正一遍：
//   · 视图里有、磁盘上没有 → 丢掉（否则卸载掉的程序图标会一直挂在桌面上）
//   · 磁盘上有、视图里没有 → 补进来（位置用哨兵 (-1,-1)，上层自己找空位摆）
// 只处理文件系统项；公共桌面的项在另一个目录里，一并算进去。
// ---------------------------------------------------------------------------
namespace {

std::wstring LowerPath(std::wstring s) {
    if (!s.empty()) CharLowerBuffW(s.data(), (DWORD)s.size());
    return s;
}

std::wstring DirOf(int csidl) {
    wchar_t buf[MAX_PATH]{};
    if (FAILED(SHGetFolderPathW(nullptr, csidl, nullptr, 0, buf))) return {};
    return std::wstring(buf);
}

void ReconcileWithDisk(std::vector<DesktopIcon>& items) {
    const std::wstring dirs[2] = {DirOf(CSIDL_DESKTOPDIRECTORY),
                                  DirOf(CSIDL_COMMON_DESKTOPDIRECTORY)};
    if (dirs[0].empty() && dirs[1].empty()) return;

    std::unordered_set<std::wstring> onDisk;   // 磁盘上真实存在（含隐藏）
    std::vector<std::wstring> addable;         // 可以摆到桌面上的
    bool allOk = true;                         // 每个目录都读到了？读不到就别删
    for (const auto& d : dirs) {
        if (d.empty()) continue;
        WIN32_FIND_DATAW fd{};
        HANDLE h = FindFirstFileW((d + L"\\*").c_str(), &fd);
        if (h == INVALID_HANDLE_VALUE) {
            allOk = false;
            continue;
        }
        do {
            if (fd.cFileName[0] == L'.' &&
                (fd.cFileName[1] == 0 || (fd.cFileName[1] == L'.' && fd.cFileName[2] == 0)))
                continue;
            const std::wstring full = d + L"\\" + fd.cFileName;
            onDisk.insert(LowerPath(full));
            // desktop.ini 是文件夹的元数据，不是图标。一般都是隐藏+系统属性，
            // 但被改过属性的机器上它会露出来，这里显式挡一道。
            if (_wcsicmp(fd.cFileName, L"desktop.ini") == 0) continue;
            if (fd.dwFileAttributes & (FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM)) continue;
            addable.push_back(full);
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    // 一个都没读到（权限 / 路径异常）：这时候「删」太危险，直接放弃校正
    if (onDisk.empty()) return;

    // 1) 磁盘上已经没了的
    //    只在每个目录都成功枚举时才删 —— 某个目录读失败时，它里面的图标会被
    //    误判成「磁盘上没有」，那样会把一整屏图标全抹掉。
    const size_t had = items.size();
    if (allOk) {
        items.erase(std::remove_if(items.begin(), items.end(),
                                   [&](const DesktopIcon& it) {
                                       if (!it.isFileSystem) return false;
                                       return onDisk.count(LowerPath(it.path)) == 0;
                                   }),
                    items.end());
    }
    const size_t removed = had - items.size();

    // 2) 磁盘上有、视图里没有的
    std::unordered_set<std::wstring> known;
    for (const auto& it : items)
        if (it.isFileSystem) known.insert(LowerPath(it.path));

    size_t added = 0;
    for (const auto& p : addable) {
        if (known.count(LowerPath(p))) continue;

        DesktopIcon icon;
        icon.path = p;
        const size_t slash = p.find_last_of(L'\\');
        icon.name = (slash == std::wstring::npos) ? p : p.substr(slash + 1);
        icon.pos = POINT{-1, -1};   // 哨兵：让上层找个空位摆，别都堆在左上角
        icon.isFileSystem = true;
        const DWORD attr = GetFileAttributesW(p.c_str());
        icon.isFolder = attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY);

        // 位置写回用的是「桌面文件夹的子 PIDL」，从完整 PIDL 里复制最后一段
        PIDLIST_ABSOLUTE full = nullptr;
        if (SUCCEEDED(SHParseDisplayName(p.c_str(), nullptr, &full, 0, nullptr)) && full) {
            PCUITEMID_CHILD child = ILFindLastID(full);
            const UINT cb = (UINT)((const BYTE*)ILGetNext(child) - (const BYTE*)child);
            auto* copy = (LPITEMIDLIST)CoTaskMemAlloc(cb);
            if (copy) {
                memcpy(copy, child, cb);
                icon.pidl = copy;
            }
            ILFree(full);
        }
        items.push_back(std::move(icon));
        ++added;
    }

    if (removed || added) {
        SetDiag("ok reconcile -%d +%d (total %d)", (int)removed, (int)added, (int)items.size());
    }
}

}  // namespace

std::vector<DesktopIcon> EnumerateDesktopIcons() {
    std::vector<DesktopIcon> result;

    IShellFolder* desktop = nullptr;
    if (FAILED(SHGetDesktopFolder(&desktop)) || !desktop) {
        SetDiag("SHGetDesktopFolder failed");
        return result;
    }

    IFolderView2* view = GetDesktopView();
    if (!view) {
        desktop->Release();
        return result;
    }

    int count = 0;
    HRESULT hrCount = view->ItemCount(SVGIO_ALLVIEW, &count);
    if (FAILED(hrCount)) {
        SetDiag("ItemCount hr=0x%08X", hrCount);
        view->Release();
        desktop->Release();
        return result;
    }

    POINT spacing{};
    view->GetSpacing(&spacing);
    if (spacing.x > 0 && spacing.y > 0) g_spacing = spacing;

    int iconSize = 48;
    FOLDERVIEWMODE mode{};
    view->GetViewModeAndIconSize(&mode, &iconSize);

    for (int i = 0; i < count; ++i) {
        LPITEMIDLIST pidl = nullptr;
        if (FAILED(view->Item(i, &pidl)) || !pidl)
            continue;

        POINT pos{};
        view->GetItemPosition(pidl, &pos);

        ULONG attrs = SFGAO_FILESYSTEM | SFGAO_FOLDER | SFGAO_STREAM;
        desktop->GetAttributesOf(1, (PCUITEMID_CHILD*)&pidl, &attrs);

        DesktopIcon icon;
        icon.path = GetNameForParsing(desktop, pidl);
        if (icon.path.empty()) {
            ILFree(pidl);
            continue;
        }
        icon.name = GetDisplayName(desktop, pidl);
        if (icon.name.empty()) icon.name = icon.path;
        icon.pos = pos;
        // 压缩包同时带 FOLDER 和 STREAM 按文件处理
        icon.isFolder = (attrs & SFGAO_FOLDER) && !(attrs & SFGAO_STREAM);
        icon.isFileSystem = (attrs & SFGAO_FILESYSTEM) && IsDesktopFolderPath(icon.path);
        icon.pidl = pidl;   // 交给调用方，用 FreeDesktopPidl 释放

        result.push_back(std::move(icon));
    }

    SetDiag("ok count=%d spacing=%dx%d iconSize=%d", count, spacing.x, spacing.y, iconSize);
    view->Release();
    desktop->Release();

    ReconcileWithDisk(result);
    return result;
}

bool SetDesktopIconPositions(size_t count, void* const* pidls, const POINT* points) {
    if (!count || !pidls || !points) return false;

    IFolderView2* view = GetDesktopView();
    if (!view) return false;

    // SVSI_POSITIONITEM：按给定坐标摆放；SVSI_NOTAKEFOCUS：别抢焦点
    const DWORD flags = SVSI_POSITIONITEM | SVSI_NOTAKEFOCUS;

    std::vector<PCUITEMID_CHILD> arr(count);
    for (size_t i = 0; i < count; ++i) {
        arr[i] = reinterpret_cast<PCUITEMID_CHILD>(pidls[i]);
    }

    HRESULT hr = view->SelectAndPositionItems(
            (UINT)count, arr.data(), const_cast<POINT*>(points), flags);
    view->Release();

    if (FAILED(hr)) {
        SetDiag("SelectAndPositionItems hr=0x%08X", hr);
        return false;
    }
    return true;
}

void FreeDesktopPidl(void* pidl) {
    if (pidl) ILFree(reinterpret_cast<LPITEMIDLIST>(pidl));
}

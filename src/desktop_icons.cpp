// 桌面图标枚举 / 位置写回：跨进程操作资源管理器的桌面视图
// 机制（对齐 my-desktop）：IShellWindows -> IShellBrowser -> IFolderView2，
// 拿 PIDL / 显示名 / 屏幕位置，并用 SelectAndPositionItems 写回新位置。
#include "desktop_icons.h"

#include <shlwapi.h>
#include <objbase.h>
#include <shobjidl.h>
#include <exdisp.h>

#include <cstdio>

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

// meow-desk 安装程序
//
// 设计取舍：
//   · 不嵌资源。安装包需要把 meow-desk.exe 一起分发，这里从「setup.exe 所在目录」
//     读它并复制到用户选定的安装位置。这样 build 产物直接可用，不必在 CMake 里
//     造一条「先构建主程序、再把 exe 打进 rc」的依赖链（那条链在增量构建里很容易
//     出现「改了主程序但安装包还是旧的」）。
//     packaging: 见 package.sh，把两个 exe 一起拷到 dist/。
//   · 只改「数据存哪儿」这一件事：便携模式在 exe 同目录留一个 meow-desk.portable
//     标记文件，主程序的 storage::Dir() 看到它就改用 exe 目录；否则用
//     %APPDATA%\meow-desk（和之前完全一致，不破坏已有数据）。
//   · 装到 %LOCALAPPDATA%\Programs\meow-desk，不需要管理员权限。
#include <windows.h>
#include <commctrl.h>
#include <dwmapi.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shlwapi.h>

#include "app_res.h"   // IDI_APP（res/ 在 meow-setup 的 include 路径里）

#include <cstring>
#include <string>

#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "shlwapi.lib")
#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "dwmapi.lib")

#pragma comment(linker, "/manifestdependency:\"type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")

namespace {

// ---- 控件 ----
enum : int {
    IDC_TITLE = 100,
    IDC_PATH = 1001,
    IDC_BROWSE = 1002,
    IDC_PORTABLE = 1003,   // 数据放 exe 同目录
    IDC_APPDATA = 1004,    // 数据放 %APPDATA%\meow-desk
    IDC_G1 = 1005,         // 分组框
    IDC_INSTALL = 1006,
    IDC_CANCEL = 1007,
    IDC_PROGRESS = 1008,
    IDC_STATUS = 1009,
    IDC_HINT = 1010,
};

const COLORREF kBg = RGB(247, 248, 251);
const COLORREF kFg = RGB(28, 30, 36);
const COLORREF kFgDim = RGB(110, 114, 124);
const COLORREF kAccent = RGB(0, 103, 192);

HWND g_path = nullptr, g_browse = nullptr, g_portable = nullptr, g_appdata = nullptr;
HWND g_group = nullptr, g_install = nullptr, g_cancel = nullptr;
HWND g_progress = nullptr, g_status = nullptr, g_hint = nullptr;
HFONT g_font = nullptr, g_fontBold = nullptr;
bool g_installed = false;
std::wstring g_installedExe;

// ---------------------------------------------------------------------------
// 路径工具
// ---------------------------------------------------------------------------
std::wstring ExeDir() {
    wchar_t buf[MAX_PATH * 2]{};
    GetModuleFileNameW(nullptr, buf, MAX_PATH * 2);
    std::wstring p = buf;
    const size_t k = p.find_last_of(L'\\');
    return k == std::wstring::npos ? p : p.substr(0, k);
}

std::wstring ExeName() {
    wchar_t buf[MAX_PATH * 2]{};
    GetModuleFileNameW(nullptr, buf, MAX_PATH * 2);
    std::wstring p = buf;
    const size_t k = p.find_last_of(L'\\');
    return k == std::wstring::npos ? p : p.substr(k + 1);
}

std::wstring KnownFolder(REFKNOWNFOLDERID id) {
    PWSTR p = nullptr;
    std::wstring out;
    if (SUCCEEDED(SHGetKnownFolderPath(id, 0, nullptr, &p)) && p) {
        out = p;
        CoTaskMemFree(p);
    }
    return out;
}

std::wstring DefaultInstallDir() {
    std::wstring base = KnownFolder(FOLDERID_LocalAppData);
    if (base.empty()) base = ExeDir();
    return base + L"\\Programs\\meow-desk";
}

bool EnsureDir(const std::wstring& dir) {
    if (dir.empty()) return false;
    if (GetFileAttributesW(dir.c_str()) != INVALID_FILE_ATTRIBUTES) return true;
    // 逐级创建（安装路径通常是多层新目录）
    std::wstring cur;
    for (size_t i = 0; i < dir.size(); ++i) {
        cur += dir[i];
        if (dir[i] == L'\\' || i + 1 == dir.size()) {
            if (cur.size() > 3) CreateDirectoryW(cur.c_str(), nullptr);
        }
    }
    return GetFileAttributesW(dir.c_str()) != INVALID_FILE_ATTRIBUTES;
}

void SetStatus(const wchar_t* text, int percent) {
    if (g_status) SetWindowTextW(g_status, text);
    if (g_progress && percent >= 0) SendMessageW(g_progress, PBM_SETPOS, percent, 0);
    // 安装是同步的：手动把界面刷一遍，否则用户看到的是"卡住"
    UpdateWindow(g_status ? g_status : nullptr);
    MSG msg;
    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
}

// 开始菜单 / 桌面上的快捷方式
bool CreateShortcut(const std::wstring& exe, const std::wstring& lnk) {
    IShellLinkW* link = nullptr;
    if (FAILED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER,
                                IID_IShellLinkW, (void**)&link)))
        return false;
    bool ok = false;
    link->SetPath(exe.c_str());
    link->SetDescription(L"meow-desk \u8f7b\u91cf\u684c\u9762\u5206\u533a\u5de5\u5177");
    std::wstring work = exe;
    const size_t k = work.find_last_of(L'\\');
    if (k != std::wstring::npos) work.resize(k);
    link->SetWorkingDirectory(work.c_str());
    link->SetIconLocation(exe.c_str(), 0);

    IPersistFile* pf = nullptr;
    if (SUCCEEDED(link->QueryInterface(IID_IPersistFile, (void**)&pf))) {
        ok = SUCCEEDED(pf->Save(lnk.c_str(), TRUE));
        pf->Release();
    }
    link->Release();
    return ok;
}

// ---------------------------------------------------------------------------
// 真正的安装动作
// ---------------------------------------------------------------------------
std::wstring ReadPath() {
    wchar_t buf[MAX_PATH * 2]{};
    GetWindowTextW(g_path, buf, MAX_PATH * 2);
    return buf;
}

bool PortableSelected() {
    return SendMessageW(g_portable, BM_GETCHECK, 0, 0) == BST_CHECKED;
}

void DoInstall(HWND hwnd) {
    const std::wstring dir = ReadPath();
    if (dir.empty()) {
        MessageBoxW(hwnd, L"\u8bf7\u5148\u9009\u62e9\u5b89\u88c5\u4f4d\u7f6e\u3002",
                    L"meow-desk", MB_ICONWARNING);
        return;
    }

    const std::wstring src = ExeDir() + L"\\meow-desk.exe";
    if (GetFileAttributesW(src.c_str()) == INVALID_FILE_ATTRIBUTES) {
        MessageBoxW(hwnd,
                    L"\u627e\u4e0d\u5230 meow-desk.exe\u3002\n\n"
                    L"\u8bf7\u628a\u5b89\u88c5\u7a0b\u5e8f\u548c meow-desk.exe "
                    L"\u653e\u5728\u540c\u4e00\u4e2a\u6587\u4ef6\u5939\u91cc\u518d\u8fd0\u884c\u3002",
                    L"meow-desk", MB_ICONERROR);
        return;
    }

    EnableWindow(g_install, FALSE);
    EnableWindow(g_browse, FALSE);

    SetStatus(L"\u6b63\u5728\u521b\u5efa\u5b89\u88c5\u76ee\u5f55\u2026", 10);
    if (!EnsureDir(dir)) {
        SetStatus(L"\u521b\u5efa\u76ee\u5f55\u5931\u8d25", 0);
        MessageBoxW(hwnd,
                    (L"\u65e0\u6cd5\u521b\u5efa\u76ee\u5f55\uff1a\n" + dir).c_str(),
                    L"meow-desk", MB_ICONERROR);
        EnableWindow(g_install, TRUE);
        EnableWindow(g_browse, TRUE);
        return;
    }

    SetStatus(L"\u6b63\u5728\u590d\u5236\u7a0b\u5e8f\u6587\u4ef6\u2026", 35);
    const std::wstring dst = dir + L"\\meow-desk.exe";
    if (!CopyFileW(src.c_str(), dst.c_str(), FALSE)) {
        SetStatus(L"\u590d\u5236\u5931\u8d25", 0);
        MessageBoxW(hwnd,
                    (L"\u65e0\u6cd5\u5199\u5165\uff1a\n" + dst +
                     L"\n\n\u5982\u679c meow-desk \u6b63\u5728\u8fd0\u884c\uff0c"
                     L"\u8bf7\u5148\u9000\u51fa\u5b83\u3002")
                        .c_str(),
                    L"meow-desk", MB_ICONERROR);
        EnableWindow(g_install, TRUE);
        EnableWindow(g_browse, TRUE);
        return;
    }

    // 数据位置：便携模式写标记文件，否则确保没有标记（回到 %APPDATA%）
    const std::wstring marker = dir + L"\\meow-desk.portable";
    if (PortableSelected()) {
        SetStatus(L"\u6b63\u5728\u5207\u6362\u5230\u4fbf\u643a\u6a21\u5f0f\u2026", 55);
        HANDLE f = CreateFileW(marker.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                               FILE_ATTRIBUTE_NORMAL, nullptr);
        if (f != INVALID_HANDLE_VALUE) {
            const char* note =
                "meow-desk portable mode\r\n"
                "config.ini / meow-desk.db are stored next to meow-desk.exe.\r\n"
                "Delete this file to go back to %APPDATA%\\meow-desk.\r\n";
            DWORD wrote = 0;
            WriteFile(f, note, (DWORD)strlen(note), &wrote, nullptr);
            CloseHandle(f);
        }

        // 老用户升级过来时，把 %APPDATA% 里已有的数据搬一份过去，
        // 否则第一次启动会看到"桌面布局全没了"
        const std::wstring oldDb = KnownFolder(FOLDERID_RoamingAppData) + L"\\meow-desk\\meow-desk.db";
        const std::wstring newDb = dir + L"\\meow-desk.db";
        if (GetFileAttributesW(oldDb.c_str()) != INVALID_FILE_ATTRIBUTES &&
            GetFileAttributesW(newDb.c_str()) == INVALID_FILE_ATTRIBUTES) {
            SetStatus(L"\u6b63\u5728\u8fc1\u79fb\u65e7\u6570\u636e\u2026", 65);
            CopyFileW(oldDb.c_str(), newDb.c_str(), FALSE);
        }
    } else {
        DeleteFileW(marker.c_str());
    }

    SetStatus(L"\u6b63\u5728\u521b\u5efa\u5feb\u6377\u65b9\u5f0f\u2026", 80);
    {
        const std::wstring menu = KnownFolder(FOLDERID_Programs);   // 开始菜单\程序
        if (!menu.empty() && EnsureDir(menu))
            CreateShortcut(dst, menu + L"\\meow-desk.lnk");

        // 桌面快捷方式（用户要求：装完就能在桌面直接点开）
        const std::wstring desk = KnownFolder(FOLDERID_Desktop);
        if (!desk.empty())
            CreateShortcut(dst, desk + L"\\meow-desk.lnk");
    }

    SetStatus(L"\u5b89\u88c5\u5b8c\u6210", 100);
    g_installed = true;
    g_installedExe = dst;

    SetWindowTextW(g_install, L"\u7acb\u5373\u8fd0\u884c");
    EnableWindow(g_install, TRUE);
    EnableWindow(g_cancel, TRUE);
    SetWindowTextW(g_cancel, L"\u5b8c\u6210");
    SetWindowTextW(g_hint,
                   (L"\u5df2\u5b89\u88c5\u5230 " + dir +
                    (PortableSelected() ? L"\uff08\u6570\u636e\u4e0e\u7a0b\u5e8f\u540c\u76ee\u5f55\uff09"
                                        : L"\uff08\u6570\u636e\u5b58\u5728 %APPDATA%\\meow-desk\uff09"))
                       .c_str());
}

// ---------------------------------------------------------------------------
// 界面
// ---------------------------------------------------------------------------
void MakeFonts() {
    NONCLIENTMETRICSW ncm{};
    ncm.cbSize = sizeof(ncm);
    SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0);
    g_font = CreateFontIndirectW(&ncm.lfMessageFont);
    LOGFONTW lf = ncm.lfMessageFont;
    lf.lfWeight = FW_SEMIBOLD;
    lf.lfHeight = -22;
    g_fontBold = CreateFontIndirectW(&lf);
}

HWND Child(HWND parent, const wchar_t* cls, const wchar_t* text, DWORD style, int id) {
    HWND h = CreateWindowExW(0, cls, text, WS_CHILD | WS_VISIBLE | style, 0, 0, 10, 10, parent,
                             (HMENU)(INT_PTR)id, nullptr, nullptr);
    if (h) SendMessageW(h, WM_SETFONT, (WPARAM)g_font, TRUE);
    return h;
}

void CreateControls(HWND h) {
    MakeFonts();

    g_path = Child(h, L"EDIT", L"", WS_TABSTOP | ES_AUTOHSCROLL, IDC_PATH);
    SendMessageW(g_path, WM_SETFONT, (WPARAM)g_font, TRUE);
    SetWindowTextW(g_path, DefaultInstallDir().c_str());

    g_browse = Child(h, L"BUTTON", L"\u6d4f\u89c8\u2026", WS_TABSTOP | BS_PUSHBUTTON, IDC_BROWSE);

    g_group = Child(h, L"BUTTON", L"\u6570\u636e\u5b58\u50a8\u4f4d\u7f6e",
                    BS_GROUPBOX, IDC_G1);

    g_portable = Child(h, L"BUTTON",
                       L"\u4e0e\u7a0b\u5e8f\u540c\u76ee\u5f55\uff08\u4fbf\u643a\u6a21\u5f0f\uff0c"
                       L"\u4e0d\u5f80\u7cfb\u7edf\u5176\u4ed6\u5730\u65b9\u5199\u4e1c\u897f\uff09",
                       WS_TABSTOP | BS_AUTORADIOBUTTON | BS_MULTILINE, IDC_PORTABLE);

    g_appdata = Child(h, L"BUTTON",
                      L"%APPDATA%\\meow-desk\uff08\u9ed8\u8ba4\uff0c"
                      L"\u4e0e\u4e4b\u524d\u7248\u672c\u4e00\u81f4\uff09",
                      WS_TABSTOP | BS_AUTORADIOBUTTON | BS_MULTILINE, IDC_APPDATA);
    SendMessageW(g_appdata, BM_SETCHECK, BST_CHECKED, 0);   // 默认保持现状

    g_progress = Child(h, PROGRESS_CLASSW, L"", PBS_SMOOTH, IDC_PROGRESS);
    SendMessageW(g_progress, PBM_SETRANGE, 0, MAKELPARAM(0, 100));
    SendMessageW(g_progress, PBM_SETSTATE, PBST_NORMAL, 0);

    g_status = Child(h, L"STATIC", L"\u51c6\u5907\u5b89\u88c5", SS_LEFT, IDC_STATUS);
    g_hint = Child(h, L"STATIC", L"", SS_LEFT | SS_ENDELLIPSIS, IDC_HINT);

    g_install = Child(h, L"BUTTON", L"\u5b89\u88c5", WS_TABSTOP | BS_DEFPUSHBUTTON, IDC_INSTALL);
    g_cancel = Child(h, L"BUTTON", L"\u53d6\u6d88", WS_TABSTOP | BS_PUSHBUTTON, IDC_CANCEL);

    // 字体
    SendMessageW(g_status, WM_SETFONT, (WPARAM)g_font, TRUE);
    SendMessageW(g_hint, WM_SETFONT, (WPARAM)g_font, TRUE);
    SendMessageW(g_install, WM_SETFONT, (WPARAM)g_font, TRUE);
    SendMessageW(g_cancel, WM_SETFONT, (WPARAM)g_font, TRUE);

    SetFocus(g_install);
}

void LayoutChildren(HWND h) {
    RECT rc{};
    GetClientRect(h, &rc);
    const int W = rc.right;
    const int pad = 26;

    MoveWindow(g_path, pad, 96, W - pad * 2 - 96, 30, TRUE);
    MoveWindow(g_browse, W - pad - 84, 96, 84, 30, TRUE);

    MoveWindow(g_group, pad, 148, W - pad * 2, 116, TRUE);
    MoveWindow(g_portable, pad + 16, 176, W - pad * 2 - 34, 28, TRUE);
    MoveWindow(g_appdata, pad + 16, 212, W - pad * 2 - 34, 28, TRUE);

    MoveWindow(g_progress, pad, 288, W - pad * 2, 8, TRUE);
    MoveWindow(g_status, pad, 302, W - pad * 2, 22, TRUE);
    MoveWindow(g_hint, pad, 324, W - pad * 2, 40, TRUE);

    MoveWindow(g_cancel, W - pad - 96, 372, 96, 34, TRUE);
    MoveWindow(g_install, W - pad - 96 - 10 - 112, 372, 112, 34, TRUE);
}

void PickFolder(HWND h) {
    BROWSEINFOW bi{};
    bi.hwndOwner = h;
    bi.lpszTitle = L"\u9009\u62e9 meow-desk \u7684\u5b89\u88c5\u4f4d\u7f6e";
    bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE | BIF_USENEWUI;
    PIDLIST_ABSOLUTE pidl = SHBrowseForFolderW(&bi);
    if (!pidl) return;
    wchar_t buf[MAX_PATH * 2]{};
    if (SHGetPathFromIDListW(pidl, buf)) {
        std::wstring p = buf;
        // 用户可能选了父目录，装到它下面的 meow-desk 里
        if (p.size() < 9 || p.compare(p.size() - 9, 9, L"meow-desk") != 0) {
            if (p.back() != L'\\') p += L'\\';
            p += L"meow-desk";
        }
        SetWindowTextW(g_path, p.c_str());
    }
    CoTaskMemFree(pidl);
}

void PaintWindow(HWND h) {
    PAINTSTRUCT ps{};
    HDC dc = BeginPaint(h, &ps);
    RECT rc{};
    GetClientRect(h, &rc);

    HBRUSH bg = CreateSolidBrush(kBg);
    FillRect(dc, &rc, bg);
    DeleteObject(bg);

    SetBkMode(dc, TRANSPARENT);

    // 左侧一条强调色竖线 + 标题
    RECT bar{26, 26, 30, 74};
    HBRUSH accent = CreateSolidBrush(kAccent);
    FillRect(dc, &bar, accent);
    DeleteObject(accent);

    HGDIOBJ old = SelectObject(dc, g_fontBold);
    SetTextColor(dc, kFg);
    RECT tr{44, 24, rc.right - 26, 56};
    DrawTextW(dc, L"meow-desk \u5b89\u88c5\u7a0b\u5e8f", -1, &tr,
              DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX);

    SelectObject(dc, g_font);
    SetTextColor(dc, kFgDim);
    RECT sr{46, 58, rc.right - 26, 82};
    DrawTextW(dc,
              L"\u8f7b\u91cf\u684c\u9762\u5206\u533a\u5de5\u5177 \u00b7 "
              L"\u5185\u5b58\u5360\u7528 < 10MB",
              -1, &sr, DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX);

    SetTextColor(dc, kFg);
    RECT lr{26, 74, rc.right - 26, 94};
    DrawTextW(dc, L"\u5b89\u88c5\u4f4d\u7f6e", -1, &lr,
              DT_LEFT | DT_SINGLELINE | DT_BOTTOM | DT_NOPREFIX);

    SelectObject(dc, old);
    EndPaint(h, &ps);
}

LRESULT CALLBACK WndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
    case WM_CREATE:
        CreateControls(h);
        LayoutChildren(h);
        return 0;

    case WM_SIZE:
        LayoutChildren(h);
        return 0;

    case WM_ERASEBKGND:
        return 1;   // WM_PAINT 里整块重画，不闪

    case WM_PAINT:
        PaintWindow(h);
        return 0;

    case WM_CTLCOLORSTATIC: {
        HDC dc = (HDC)w;
        SetBkColor(dc, kBg);
        SetTextColor(dc, kFg);
        static HBRUSH br = CreateSolidBrush(kBg);
        return (LRESULT)br;
    }
    case WM_CTLCOLORBTN: {
        static HBRUSH br = CreateSolidBrush(kBg);
        return (LRESULT)br;
    }
    case WM_CTLCOLOREDIT: {
        static HBRUSH br = CreateSolidBrush(RGB(255, 255, 255));
        HDC dc = (HDC)w;
        SetBkColor(dc, RGB(255, 255, 255));
        SetTextColor(dc, kFg);
        return (LRESULT)br;
    }

    case WM_COMMAND:
        switch (LOWORD(w)) {
        case IDC_BROWSE:
            PickFolder(h);
            return 0;
        case IDC_INSTALL:
            if (g_installed) {
                if (!g_installedExe.empty())
                    ShellExecuteW(nullptr, L"open", g_installedExe.c_str(), nullptr, nullptr,
                                  SW_SHOWNORMAL);
                DestroyWindow(h);
            } else {
                DoInstall(h);
            }
            return 0;
        case IDC_CANCEL:
            DestroyWindow(h);
            return 0;
        case IDC_PORTABLE:
            SendMessageW(g_appdata, BM_SETCHECK, BST_UNCHECKED, 0);
            return 0;
        case IDC_APPDATA:
            SendMessageW(g_portable, BM_SETCHECK, BST_UNCHECKED, 0);
            return 0;
        default:
            break;
        }
        return 0;

    case WM_CLOSE:
        if (g_installed) {
            DestroyWindow(h);
        } else {
            // 装到一半/还没装，确认一下再退
            if (MessageBoxW(h, L"\u786e\u5b9a\u8981\u53d6\u6d88\u5b89\u88c5\u5417\uff1f",
                            L"meow-desk", MB_YESNO | MB_ICONQUESTION) == IDYES)
                DestroyWindow(h);
        }
        return 0;

    case WM_DESTROY:
        if (g_font) DeleteObject(g_font);
        if (g_fontBold) DeleteObject(g_fontBold);
        PostQuitMessage(0);
        return 0;
    default:
        break;
    }
    return DefWindowProcW(h, m, w, l);
}

}  // namespace

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR, int show) {
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    INITCOMMONCONTROLSEX icc{sizeof(icc), ICC_PROGRESS_CLASS | ICC_STANDARD_CLASSES};
    InitCommonControlsEx(&icc);

    // 同一个安装包不要开两份
    CreateMutexW(nullptr, TRUE, L"meow-desk-setup-singleton");
    if (GetLastError() == ERROR_ALREADY_EXISTS) return 0;

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = WndProc;
    wc.hInstance = inst;
    wc.lpszClassName = L"MeowDeskSetup";
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    // 窗口 / 任务栏图标：用资源里的 IDI_APP。
    // 以前这里写的是 LoadIconW(inst, L"IDI_APPLICATION") —— 那是把宏当成**字符串**
    // 资源名传进去了，我们根本没有叫这个名字的图标，于是永远拿到 NULL，
    // 安装器窗口和任务栏就只能显示系统默认图标。
    const int cxBig = GetSystemMetrics(SM_CXICON), cyBig = GetSystemMetrics(SM_CYICON);
    const int cxSm = GetSystemMetrics(SM_CXSMICON), cySm = GetSystemMetrics(SM_CYSMICON);
    wc.hIcon = (HICON)LoadImageW(inst, MAKEINTRESOURCEW(IDI_APP), IMAGE_ICON, cxBig, cyBig,
                                LR_DEFAULTCOLOR);
    wc.hIconSm = (HICON)LoadImageW(inst, MAKEINTRESOURCEW(IDI_APP), IMAGE_ICON, cxSm, cySm,
                                  LR_DEFAULTCOLOR);
    RegisterClassExW(&wc);

    // 客户区高度 = 最下面一排按钮的底边(406) + 10px 内边距
    const int W = 560, H = 416;
    const int sw = GetSystemMetrics(SM_CXSCREEN), sh = GetSystemMetrics(SM_CYSCREEN);
    RECT rc{0, 0, W, H};
    AdjustWindowRectEx(&rc, WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU, FALSE, 0);
    const int ww = rc.right - rc.left, wh = rc.bottom - rc.top;

    HWND h = CreateWindowExW(
        0, L"MeowDeskSetup", L"meow-desk \u5b89\u88c5\u7a0b\u5e8f",
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_CLIPCHILDREN,
        (sw - ww) / 2, (sh - wh) / 2, ww, wh, nullptr, nullptr, inst, nullptr);
    if (!h) return 1;

    // Win11 圆角（旧系统调用失败就算了）
    enum { DWMWA_WINDOW_CORNER_PREFERENCE_ = 33 };
    const int round = 2;   // DWMWCP_ROUND
    DwmSetWindowAttribute(h, DWMWA_WINDOW_CORNER_PREFERENCE_, &round, sizeof(round));

    ShowWindow(h, show ? show : SW_SHOW);
    UpdateWindow(h);

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (!IsDialogMessageW(h, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }
    CoUninitialize();
    return 0;
}

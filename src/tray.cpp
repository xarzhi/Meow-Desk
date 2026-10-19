#include "tray.h"

#include <shellapi.h>
#include <objidl.h>
#include <propidl.h>
#include <gdiplus.h>

#include "icon_render.h"
#include "app_icon.h"

#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "gdiplus.lib")

namespace {

const wchar_t* kTrayClass = L"MeowDeskTray";
const UINT kCallbackMsg = WM_APP + 100;
const UINT kTrayId = 1;

enum : UINT {
    ID_SETTINGS = 1001,
    ID_RELOAD = 1002,
    ID_DESKTOP_ICONS = 1003,
    ID_EXIT = 1004,
};

// 托盘图标：优先用 exe 资源里的 app 图标（res/meow-desk.ico）；
// 资源没编进去时退回「青色圆角方块 + 2x2 白点」——宁可样子朴素，也不要一个空白方块。
HICON MakeTrayIcon(int size) {
    if (HICON ico = LoadAppIcon(size)) return ico;

    EnsureGdiplus();
    using namespace Gdiplus;

    Bitmap bmp(size, size, PixelFormat32bppARGB);
    {
        Graphics g(&bmp);
        g.SetSmoothingMode(SmoothingModeAntiAlias);
        g.SetPixelOffsetMode(PixelOffsetModeHighQuality);
        g.Clear(Color(0, 0, 0, 0));

        const float s = (float)size;
        GraphicsPath p;
        const float r = s * 0.24f, d = r * 2.0f;
        p.AddArc(0.0f, 0.0f, d, d, 180.0f, 90.0f);
        p.AddArc(s - d, 0.0f, d, d, 270.0f, 90.0f);
        p.AddArc(s - d, s - d, d, d, 0.0f, 90.0f);
        p.AddArc(0.0f, s - d, d, d, 90.0f, 90.0f);
        p.CloseFigure();

        LinearGradientBrush bg(PointF(0, 0), PointF(s, s),
                               Color(255, 56, 189, 248), Color(255, 14, 116, 214));
        g.FillPath(&bg, &p);

        SolidBrush dot{Color(255, 255, 255, 255)};
        const float r2 = s * 0.115f;
        const float cx[2] = {s * 0.33f, s * 0.67f};
        const float cy[2] = {s * 0.33f, s * 0.67f};
        for (float y : cy)
            for (float x : cx) g.FillEllipse(&dot, x - r2, y - r2, r2 * 2, r2 * 2);
        g.Flush(FlushIntentionSync);
    }

    HICON ico = nullptr;
    bmp.GetHICON(&ico);
    return ico;
}

}  // namespace

LRESULT CALLBACK TrayIcon::WndProcStatic(HWND h, UINT m, WPARAM w, LPARAM l) {
    TrayIcon* self = reinterpret_cast<TrayIcon*>(GetWindowLongPtrW(h, GWLP_USERDATA));
    if (m == WM_NCCREATE) {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(l);
        self = reinterpret_cast<TrayIcon*>(cs->lpCreateParams);
        SetWindowLongPtrW(h, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    }
    if (self) return self->WndProc(h, m, w, l);
    return DefWindowProcW(h, m, w, l);
}

LRESULT TrayIcon::WndProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == kCallbackMsg) {
        switch (LOWORD(lp)) {
        case WM_RBUTTONUP:
        case WM_CONTEXTMENU:
            ShowMenu();
            return 0;
        case WM_LBUTTONDBLCLK:
            if (onSettings) onSettings();
            return 0;
        default:
            return 0;
        }
    }
    if (msg == WM_COMMAND) {
        switch (LOWORD(wp)) {
        case ID_SETTINGS: if (onSettings) onSettings(); return 0;
        case ID_RELOAD: if (onReloadIcons) onReloadIcons(); return 0;
        case ID_DESKTOP_ICONS: if (onToggleDesktopIcons) onToggleDesktopIcons(); return 0;
        case ID_EXIT: if (onExit) onExit(); return 0;
        default: return 0;
        }
    }
    if (msg == WM_DESTROY) return 0;
    return DefWindowProcW(h, msg, wp, lp);
}

void TrayIcon::ShowMenu() {
    HMENU menu = CreatePopupMenu();
    AppendMenuW(menu, MF_STRING, ID_SETTINGS, L"\u6253\u5f00\u8bbe\u7f6e");
    AppendMenuW(menu, MF_STRING, ID_RELOAD, L"\u5237\u65b0\u684c\u9762\u56fe\u6807");
    AppendMenuW(menu, MF_STRING | (desktopIconsVisible_ ? MF_CHECKED : MF_UNCHECKED),
                ID_DESKTOP_ICONS, L"\u663e\u793a\u7cfb\u7edf\u684c\u9762\u56fe\u6807");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, ID_EXIT, L"\u9000\u51fa");

    POINT pt{};
    GetCursorPos(&pt);
    SetForegroundWindow(hwnd_);
    TrackPopupMenu(menu, TPM_RIGHTBUTTON | TPM_BOTTOMALIGN, pt.x, pt.y, 0, hwnd_, nullptr);
    DestroyMenu(menu);
    PostMessageW(hwnd_, WM_NULL, 0, 0);
}

bool TrayIcon::Create(HWND owner) {
    owner_ = owner;

    HINSTANCE inst = GetModuleHandleW(nullptr);
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = WndProcStatic;
    wc.hInstance = inst;
    wc.lpszClassName = kTrayClass;
    RegisterClassExW(&wc);

    hwnd_ = CreateWindowExW(0, kTrayClass, L"", WS_POPUP, 0, 0, 0, 0,
                            nullptr, nullptr, inst, this);
    if (!hwnd_) return false;

    NOTIFYICONDATAW nid{};
    nid.cbSize = sizeof(nid);
    nid.hWnd = hwnd_;
    nid.uID = kTrayId;
    nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    nid.uCallbackMessage = kCallbackMsg;
    nid.hIcon = MakeTrayIcon(32);
    wcscpy_s(nid.szTip, L"Meow Desk \u00b7 \u684c\u9762\u5206\u533a");
    added_ = Shell_NotifyIconW(NIM_ADD, &nid) != FALSE;
    if (nid.hIcon) DestroyIcon(nid.hIcon);
    return added_;
}

void TrayIcon::Destroy() {
    if (added_) {
        NOTIFYICONDATAW nid{};
        nid.cbSize = sizeof(nid);
        nid.hWnd = hwnd_;
        nid.uID = kTrayId;
        Shell_NotifyIconW(NIM_DELETE, &nid);
        added_ = false;
    }
    if (hwnd_) {
        SetWindowLongPtrW(hwnd_, GWLP_USERDATA, 0);
        DestroyWindow(hwnd_);
        hwnd_ = nullptr;
    }
}

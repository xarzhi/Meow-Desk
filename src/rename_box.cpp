#include "rename_box.h"

#include "icon_render.h"

#include <commctrl.h>

#include <utility>

namespace RenameBox {
namespace {

HWND g_hwnd = nullptr;
HWND g_edit = nullptr;
std::function<void(const std::wstring&)> g_cb;

const wchar_t* kClass = L"MeowDeskRenameBox";

void Finish(bool commit) {
    if (!g_hwnd) return;

    std::wstring text;
    if (commit && g_edit) {
        const int n = GetWindowTextLengthW(g_edit);
        if (n > 0) {
            text.resize((size_t)n + 1);
            GetWindowTextW(g_edit, text.data(), n + 1);
            text.resize((size_t)n);
        }
    }

    // 先把状态清干净、再销毁窗口：销毁过程中会再收到 WM_ACTIVATE(WA_INACTIVE)，
    // 不清的话会递归回到这里。
    HWND h = g_hwnd;
    g_hwnd = nullptr;
    g_edit = nullptr;
    auto cb = std::move(g_cb);
    g_cb = nullptr;

    if (h) DestroyWindow(h);   // 子 EDIT 随之销毁

    // 回调放在窗口销毁之后：里面会重新枚举图标、可能弹确认框，不能在这些
    // 消息处理过程中把窗口拆掉。
    if (commit && !text.empty() && cb) cb(text);
}

LRESULT CALLBACK EditProc(HWND h, UINT m, WPARAM w, LPARAM l, UINT_PTR, DWORD_PTR) {
    switch (m) {
    case WM_KEYDOWN:
        if (w == VK_RETURN) {
            Finish(true);
            return 0;
        }
        if (w == VK_ESCAPE) {
            Finish(false);
            return 0;
        }
        break;
    case WM_CHAR:
        // 单行 EDIT 收到回车会「叮」一声；Esc 也会。自己处理掉了就别再响。
        if (w == L'\r' || w == L'\n' || w == 27) return 0;
        break;
    case WM_GETDLGCODE:
        return DLGC_WANTALLKEYS;
    case WM_NCDESTROY:
        RemoveWindowSubclass(h, EditProc, 1);
        break;
    }
    return DefSubclassProc(h, m, w, l);
}

LRESULT CALLBACK BoxProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
    case WM_ERASEBKGND: {
        RECT r{};
        GetClientRect(h, &r);
        FillRect((HDC)w, &r, (HBRUSH)GetStockObject(WHITE_BRUSH));
        return 1;
    }
    case WM_ACTIVATE:
        if (LOWORD(w) == WA_INACTIVE) {
            Finish(true);   // 点到别处 = 确认，和资源管理器一致
            return 0;
        }
        break;
    case WM_CLOSE:
        Finish(false);
        return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

void EnsureClass() {
    static bool done = false;
    if (done) return;
    done = true;
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = BoxProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.hCursor = LoadCursorW(nullptr, IDC_IBEAM);
    wc.lpszClassName = kClass;
    RegisterClassExW(&wc);
}

}  // namespace

bool IsOpen() { return g_hwnd != nullptr; }

void Close() { Finish(false); }

void Show(HWND owner, RECT labelRect, const std::wstring& currentName,
          std::function<void(const std::wstring&)> onCommit) {
    Close();
    EnsureClass();

    const int lineH = IconLabelLineHeight();
    int w = labelRect.right - labelRect.left;
    if (w < 170) w = 170;
    const int hgt = lineH + 12;

    // 贴着标签那一行弹出：先按标签的左边缘对齐，超出屏幕右边就往左挪回来
    int x = labelRect.left;
    int y = labelRect.top;
    RECT wa{};
    if (SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0)) {
        if (x + w > wa.right) x = wa.right - w;
        if (x < wa.left) x = wa.left;
        if (y + hgt > wa.bottom) y = wa.bottom - hgt;
        if (y < wa.top) y = wa.top;
    }

    HWND h = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_TOPMOST, kClass, L"",
                             WS_POPUP | WS_BORDER, x, y, w, hgt, owner, nullptr,
                             GetModuleHandleW(nullptr), nullptr);
    if (!h) return;
    g_hwnd = h;
    g_cb = std::move(onCommit);

    g_edit = CreateWindowExW(0, L"EDIT", L"", WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL, 4,
                             (hgt - (lineH + 4)) / 2, w - 8, lineH + 4, h, nullptr,
                             GetModuleHandleW(nullptr), nullptr);
    if (!g_edit) {
        Finish(false);
        return;
    }
    SendMessageW(g_edit, WM_SETFONT, (WPARAM)IconLabelFont(), TRUE);
    SetWindowSubclass(g_edit, EditProc, 1, 0);

    SetWindowTextW(g_edit, currentName.c_str());

    // 只选中主文件名：最后一个点之后算扩展名（点在最前面算是隐藏文件名，不切）
    int selEnd = (int)currentName.size();
    const size_t dot = currentName.find_last_of(L'.');
    if (dot != std::wstring::npos && dot > 0) selEnd = (int)dot;
    SendMessageW(g_edit, EM_SETSEL, 0, selEnd);

    // 我们的主窗口都是 WS_EX_NOACTIVATE，进程要做一次「真正激活」这个弹窗
    // 才拿得到键盘焦点；菜单刚关掉，前台权利还在我们手上，这三步一般都会成功。
    SetActiveWindow(h);
    SetForegroundWindow(h);
    BringWindowToTop(h);
    SetFocus(g_edit);
}

}  // namespace RenameBox

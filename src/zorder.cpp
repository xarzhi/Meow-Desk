#include "zorder.h"

#include <unordered_map>
#include <utility>

namespace {

struct Entry {
    WNDPROC orig = nullptr;
    std::function<HWND()> anchorFn;
};

std::unordered_map<HWND, Entry> g_entries;

// 看不见也点不到的窗口：隐藏窗口（含输入法给界面线程配的隐藏窗口）、
// 其他程序铺在桌面上方的鼠标穿透分层窗口（截图/贴图软件常这么干）。
// 排层级时越过它们，因为它们不挡着什么。
bool IsIgnorable(HWND h) {
    if (!IsWindowVisible(h)) return true;
    DWORD pid = 0;
    GetWindowThreadProcessId(h, &pid);
    if (pid == GetCurrentProcessId()) return false;
    LONG_PTR ex = GetWindowLongPtrW(h, GWL_EXSTYLE);
    return (ex & (WS_EX_LAYERED | WS_EX_TRANSPARENT)) == (WS_EX_LAYERED | WS_EX_TRANSPARENT);
}

// 插到 anchor 正上方时应传给 hWndInsertAfter 的窗口：
// 即 anchor 上面第一个不该越过的窗口；anchor 已在最上层时返回 nullptr（= HWND_TOP）。
HWND AnchorAbove(HWND anchor) {
    if (!anchor) return nullptr;
    HWND above = GetWindow(anchor, GW_HWNDPREV);
    while (above && IsIgnorable(above)) above = GetWindow(above, GW_HWNDPREV);
    return above;
}

LRESULT CALLBACK SubclassProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    auto it = g_entries.find(h);
    if (it == g_entries.end()) return DefWindowProcW(h, msg, wp, lp);
    WNDPROC orig = it->second.orig;

    if (msg == WM_WINDOWPOSCHANGING) {
        auto* pos = reinterpret_cast<WINDOWPOS*>(lp);
        HWND anchor = it->second.anchorFn ? it->second.anchorFn() : nullptr;
        if (!(pos->flags & SWP_NOZORDER) && anchor && IsWindow(anchor)) {
            HWND after = AnchorAbove(anchor);
            if (after == h) {
                // 已经在目标位置，别再动，避免"插在自己后面"导致调用失败
                pos->flags |= SWP_NOZORDER;
            } else if (!after) {
                pos->hwndInsertAfter = HWND_TOP;
            } else {
                pos->hwndInsertAfter = after;
            }
        }
    } else if (msg == WM_NCDESTROY) {
        g_entries.erase(it);
        LRESULT r = CallWindowProcW(orig, h, msg, wp, lp);
        return r;
    }
    return CallWindowProcW(orig, h, msg, wp, lp);
}

}  // namespace

HWND FindDesktopWindow() {
    HWND progman = FindWindowW(L"Progman", nullptr);
    if (progman && FindWindowExW(progman, nullptr, L"SHELLDLL_DefView", nullptr))
        return progman;

    struct Ctx { HWND found; } ctx{nullptr};
    EnumWindows([](HWND hwnd, LPARAM lp) -> BOOL {
        if (FindWindowExW(hwnd, nullptr, L"SHELLDLL_DefView", nullptr)) {
            reinterpret_cast<Ctx*>(lp)->found = hwnd;
            return FALSE;
        }
        return TRUE;
    }, (LPARAM)&ctx);
    return ctx.found ? ctx.found : progman;
}

void InstallKeepAbove(HWND hwnd, std::function<HWND()> anchorFn) {
    if (!hwnd || g_entries.count(hwnd)) return;
    WNDPROC orig = reinterpret_cast<WNDPROC>(
        SetWindowLongPtrW(hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&SubclassProc)));
    if (!orig) return;
    g_entries[hwnd] = Entry{orig, std::move(anchorFn)};
}

void RemoveKeepAbove(HWND hwnd) {
    auto it = g_entries.find(hwnd);
    if (it == g_entries.end()) return;
    SetWindowLongPtrW(hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(it->second.orig));
    g_entries.erase(it);
}

void PlaceAbove(HWND hwnd, HWND anchor) {
    if (!hwnd || !anchor || !IsWindow(anchor)) return;
    if (IsPinnedAbove(hwnd, anchor)) return;
    HWND after = AnchorAbove(anchor);
    if (after == hwnd) return;
    SetWindowPos(hwnd, after ? after : HWND_TOP, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
}

bool IsPinnedAbove(HWND hwnd, HWND anchor) {
    if (!hwnd || !anchor || !IsWindow(anchor)) return false;
    for (HWND w = GetWindow(anchor, GW_HWNDPREV); w; w = GetWindow(w, GW_HWNDPREV)) {
        if (w == hwnd) return true;
        if (!IsIgnorable(w)) return false;
    }
    return false;
}

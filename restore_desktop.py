"""恢复 Windows 桌面图标显示（异常退出后手动兜底）"""
import ctypes
from ctypes import wintypes

u = ctypes.windll.user32

def find_defview():
    pm = u.FindWindowW("Progman", None)
    if pm:
        dv = u.FindWindowExW(pm, None, "SHELLDLL_DefView", None)
        if dv:
            return dv
    found = []
    EnumProc = ctypes.WINFUNCTYPE(ctypes.c_bool, wintypes.HWND, wintypes.LPARAM)
    def cb(hwnd, lp):
        dv = u.FindWindowExW(hwnd, None, "SHELLDLL_DefView", None)
        if dv:
            found.append(dv)
            return False
        return True
    u.EnumWindows(EnumProc(cb), 0)
    return found[0] if found else None

dv = find_defview()
print("DefView =", dv)
if not dv:
    raise SystemExit("no SHELLDLL_DefView")
lv = u.FindWindowExW(dv, None, "SysListView32", None)
print("SysListView32 =", lv)
if lv:
    u.ShowWindowAsync(lv, 5)  # SW_SHOWNA
    print("visible ->", u.IsWindowVisible(lv))

import ctypes, time, subprocess, sys
from ctypes import wintypes

u = ctypes.windll.user32

def find_fence():
    found = []
    @ctypes.WINFUNCTYPE(ctypes.c_bool, wintypes.HWND, wintypes.LPARAM)
    def cb(hwnd, lp):
        pid = wintypes.DWORD()
        u.GetWindowThreadProcessId(hwnd, ctypes.byref(pid))
        try:
            out = subprocess.check_output(['tasklist','/FI',f'PID eq {pid.value}','/FO','CSV']).decode('gbk','ignore')
        except Exception:
            return True
        if 'meow-desk.exe' in out:
            cls = ctypes.create_unicode_buffer(128); u.GetClassNameW(hwnd, cls, 128)
            title = ctypes.create_unicode_buffer(128); u.GetWindowTextW(hwnd, title, 128)
            r = wintypes.RECT(); u.GetWindowRect(hwnd, ctypes.byref(r))
            found.append((hwnd, cls.value, title.value, r.left, r.top, r.right, r.bottom))
        return True
    u.EnumWindows(cb, 0)
    return found

wins = find_fence()
print("windows:", wins)
if not wins:
    print("NO WINDOW")
    sys.exit()

# 按标题精确取盒子窗口
hwnd = next((w[0] for w in wins if w[2] == 'meow-desk-fence'), None)
if hwnd is None:
    print("fence not found"); sys.exit()
r = wintypes.RECT(); u.GetWindowRect(hwnd, ctypes.byref(r))
dpi = u.GetDpiForWindow(hwnd)
s = dpi / 96.0
print(f"fence hwnd={hwnd} rect=({r.left},{r.top},{r.right},{r.bottom}) dpi={dpi} h={r.bottom-r.top}")

for try_x in [(340-8-15), (340-20)]:
    x = int(try_x * s); y = int(22 * s)
    lp = (y << 16) | (x & 0xFFFF)
    u.PostMessageW(hwnd, 0x0201, 1, lp)
    u.PostMessageW(hwnd, 0x0202, 0, lp)
    time.sleep(1.2)
    r2 = wintypes.RECT(); u.GetWindowRect(hwnd, ctypes.byref(r2))
    h = r2.bottom - r2.top
    print(f"click at ({x},{y}) -> height={h}")
    if h < 100:
        print("COLLAPSED OK")
        break

"""折叠/展开 + 盒子拖动跟手"""
import ctypes, os, subprocess, time
from ctypes import wintypes

ROOT = r"C:\Users\22357\Desktop\meow-desk\meow-desk-core"
EXE = os.path.join(ROOT, "build", "meow-desk.exe")
PY = r"C:\Users\22357\.workbuddy\binaries\python\envs\default\Scripts\python.exe"

try:
    ctypes.windll.shcore.SetProcessDpiAwareness(2)
except Exception:
    pass
u = ctypes.windll.user32

def click(x, y):
    u.SetCursorPos(x, y); time.sleep(0.12)
    u.mouse_event(0x0002, 0, 0, 0, 0); time.sleep(0.06)
    u.mouse_event(0x0004, 0, 0, 0, 0); time.sleep(0.5)

def fence_rects(pid):
    out = []
    EnumProc = ctypes.WINFUNCTYPE(ctypes.c_bool, wintypes.HWND, wintypes.LPARAM)
    def cb(hwnd, lp):
        q = wintypes.DWORD(); u.GetWindowThreadProcessId(hwnd, ctypes.byref(q))
        if q.value != pid: return True
        b = ctypes.create_unicode_buffer(64); u.GetClassNameW(hwnd, b, 64)
        if b.value == "MeowDeskFence":
            r = wintypes.RECT(); u.GetWindowRect(hwnd, ctypes.byref(r))
            out.append((hwnd, r.left, r.top, r.right - r.left, r.bottom - r.top))
        return True
    u.EnumWindows(EnumProc(cb), 0)
    return out

def shot(name, box):
    subprocess.run([PY, "-c",
        "from PIL import ImageGrab; ImageGrab.grab(bbox=%r).save(r'%s')"
        % (box, os.path.join(ROOT, name))], check=False)

def hide_windows(pid):
    EnumProc = ctypes.WINFUNCTYPE(ctypes.c_bool, wintypes.HWND, wintypes.LPARAM)
    hidden = []
    def cb(h, lp):
        if not u.IsWindowVisible(h) or u.IsIconic(h): return True
        q = wintypes.DWORD(); u.GetWindowThreadProcessId(h, ctypes.byref(q))
        if q.value == pid: return True
        if u.GetWindowLongW(h, -20) & 0x00000080: return True
        hidden.append(h); u.ShowWindow(h, 6); return True
    u.EnumWindows(EnumProc(cb), 0)
    return hidden

subprocess.run([PY, os.path.join(ROOT, "restore_desktop.py")], capture_output=True)
subprocess.run(["taskkill", "/F", "/IM", "meow-desk.exe"], capture_output=True)
time.sleep(0.8)
cfg = os.path.join(os.environ['APPDATA'], 'meow-desk', 'config.ini')
if os.path.exists(cfg):
    os.remove(cfg)

p = subprocess.Popen([EXE], cwd=ROOT)
time.sleep(5.0)
hidden = hide_windows(p.pid)
time.sleep(1.2)

rects = fence_rects(p.pid)
print("fences:", rects)
box = (1080, 80, 1470, 900)
shot("c0_before.png", box)

# 折叠 my 盒子（下面那个，y 更大的）
target = max(rects, key=lambda r: r[2])
h, x, y, w, hh = target
click(x + w - 21, y + 22)
time.sleep(0.8)
shot("c1_collapsed.png", box)
print("after collapse:", fence_rects(p.pid))

click(x + w - 21, y + 22)
time.sleep(1.0)
shot("c2_expanded.png", box)
print("after expand:", fence_rects(p.pid))

# 拖动盒子（用头部）
u.SetCursorPos(x + 120, y + 20); time.sleep(0.15)
u.mouse_event(0x0002, 0, 0, 0, 0); time.sleep(0.1)
for i in range(1, 13):
    u.SetCursorPos(x + 120 - 5 * i, y + 20 - 3 * i)
    time.sleep(0.02)
time.sleep(0.2)
u.mouse_event(0x0004, 0, 0, 0, 0); time.sleep(0.6)
print("after drag:", fence_rects(p.pid))
shot("c3_moved.png", (1000, 40, 1490, 900))

for h2 in reversed(hidden):
    u.ShowWindow(h2, 9)
u.PostMessageW(u.FindWindowW("MeowDeskFence", None), 0x0010, 0, 0)
time.sleep(0.4)
subprocess.run(["taskkill", "/F", "/IM", "meow-desk.exe"], capture_output=True)
subprocess.run([PY, os.path.join(ROOT, "restore_desktop.py")], capture_output=True)
print(open(cfg, encoding="utf-8-sig").read() if os.path.exists(cfg) else "no config")

"""交互测试：桌面 → 盒子 → 桌面 → 另一个盒子"""
import ctypes, os, subprocess, sys, time
from ctypes import wintypes

ROOT = r"C:\Users\22357\Desktop\meow-desk\meow-desk-core"
EXE = os.path.join(ROOT, "build", "meow-desk.exe")
PY = r"C:\Users\22357\.workbuddy\binaries\python\envs\default\Scripts\python.exe"

try:
    ctypes.windll.shcore.SetProcessDpiAwareness(2)
except Exception:
    pass

u = ctypes.windll.user32
SW, SH = u.GetSystemMetrics(0), u.GetSystemMetrics(1)
CELL_W, CELL_H = 75, 98

def move(x, y, steps=1, delay=0.012):
    for i in range(steps):
        u.SetCursorPos(int(x), int(y))
        time.sleep(delay)

def drag(x0, y0, x1, y1, steps=22):
    u.SetCursorPos(x0, y0)
    time.sleep(0.12)
    u.mouse_event(0x0002, 0, 0, 0, 0)   # LEFTDOWN
    time.sleep(0.08)
    for i in range(1, steps + 1):
        t = i / steps
        u.SetCursorPos(int(x0 + (x1 - x0) * t), int(y0 + (y1 - y0) * t))
        time.sleep(0.018)
    time.sleep(0.15)
    u.mouse_event(0x0004, 0, 0, 0, 0)   # LEFTUP
    time.sleep(0.35)

def click(x, y):
    u.SetCursorPos(x, y); time.sleep(0.1)
    u.mouse_event(0x0002, 0, 0, 0, 0); time.sleep(0.05)
    u.mouse_event(0x0004, 0, 0, 0, 0); time.sleep(0.3)

def shot(name, crop=None):
    code = "from PIL import ImageGrab; im=ImageGrab.grab(); "
    if crop:
        code += "im=im.crop(%r); " % (crop,)
    code += "im.save(r'%s')" % os.path.join(ROOT, name)
    subprocess.run([PY, "-c", code], check=False)

def icon_pos(name):
    for line in open(os.path.join(ROOT, "icons.txt"), encoding="utf-8-sig"):
        parts = line.rstrip("\n").split("\t")
        if len(parts) == 3 and parts[0] == name:
            return int(parts[1]), int(parts[2])
    return None

def hide_windows(pid):
    EnumProc = ctypes.WINFUNCTYPE(ctypes.c_bool, wintypes.HWND, wintypes.LPARAM)
    hidden = []
    def cb(hwnd, lp):
        if not u.IsWindowVisible(hwnd) or u.IsIconic(hwnd):
            return True
        q = wintypes.DWORD(); u.GetWindowThreadProcessId(hwnd, ctypes.byref(q))
        if q.value == pid:
            return True
        if u.GetWindowLongW(hwnd, -20) & 0x00000080:
            return True
        hidden.append(hwnd); u.ShowWindow(hwnd, 6)
        return True
    u.EnumWindows(EnumProc(cb), 0)
    return hidden

def show_windows(hidden):
    for h in reversed(hidden):
        u.ShowWindow(h, 9)

subprocess.run([PY, os.path.join(ROOT, "restore_desktop.py")], capture_output=True)
subprocess.run(["taskkill", "/F", "/IM", "meow-desk.exe"], capture_output=True)
time.sleep(1.0)
log = os.path.join(ROOT, "meow-desk.log")
if os.path.exists(log):
    os.remove(log)

p = subprocess.Popen([EXE], cwd=ROOT)
time.sleep(5.0)
hidden = hide_windows(p.pid)
time.sleep(1.2)

# 盒子里第一个格子的中心
FENCE_A = (1100, 400)          # 我的盒子
FENCE_B = (1100, 100)          # 工作
def cell_center(fx, fy, i):
    ox = (340 - 4 * 74) // 2
    cx = fx + ox + (i % 4) * 74 + 37
    cy = fy + 44 + 14 + (i // 4) * 62 + 31
    return cx, cy

result = {}

# 1) 桌面 → 盒子A
src = icon_pos("QQ")
print("QQ at", src)
if src:
    sx, sy = src[0] + CELL_W // 2, src[1] + 4 + 24
    dx, dy = cell_center(*FENCE_A, 0)
    drag(sx, sy, dx, dy)
    time.sleep(0.6)
    shot("t1_fenceA.png", (1080, 380, 1470, 880))
    shot("t1_desktop.png", (0, 0, 420, 300))
    result["desktop->fence"] = True

# 2) 盒子A → 盒子B（先看 A 里第一格）
sx, sy = cell_center(*FENCE_A, 0)
dx, dy = cell_center(*FENCE_B, 0)
drag(sx, sy, dx, dy)
time.sleep(0.6)
shot("t2_fenceB.png", (1080, 80, 1470, 400))
shot("t2_fenceA.png", (1080, 380, 1470, 880))

# 3) 盒子B → 桌面空白处
sx, sy = cell_center(*FENCE_B, 0)
drag(sx, sy, 600, 1150)
time.sleep(0.6)
shot("t3_desktop.png", (400, 1050, 900, 1350))
shot("t3_fenceB.png", (1080, 80, 1470, 400))

show_windows(hidden)
print("alive:", p.poll() is None)
u.PostMessageW(u.FindWindowW("MeowDeskFence", None), 0x0010, 0, 0)
time.sleep(0.3)
subprocess.run(["taskkill", "/F", "/IM", "meow-desk.exe"], capture_output=True)
subprocess.run([PY, os.path.join(ROOT, "restore_desktop.py")], capture_output=True)
print("--- log ---")
if os.path.exists(log):
    print(open(log, encoding="utf-8", errors="replace").read())

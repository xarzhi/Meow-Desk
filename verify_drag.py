"""验收：图标拖动改位置（写回真实桌面）+ 框选"""
import ctypes, os, subprocess, sys, time
from ctypes import wintypes

ROOT = r"C:\Users\22357\Desktop\meow-desk\meow-desk-core"
EXE = os.path.join(ROOT, "build", "meow-desk.exe")
PY = r"C:\Users\22357\.workbuddy\binaries\python\envs\default\Scripts\python.exe"
LOG = os.path.join(ROOT, "meow-desk.log")
u = ctypes.windll.user32

if os.path.exists(LOG):
    os.remove(LOG)

def shot(path, crop=None):
    if crop:
        code = ("from PIL import ImageGrab; im=ImageGrab.grab(); im.crop(%r).save(r'%s')" % (crop, path))
    else:
        code = ("from PIL import ImageGrab; im=ImageGrab.grab(); im.save(r'%s')" % path)
    subprocess.run([PY, "-c", code], check=False)

p = subprocess.Popen([EXE], cwd=ROOT)
time.sleep(4.5)
if p.poll() is not None:
    print("EXITED", p.returncode); sys.exit(1)

surface = u.FindWindowW("MeowDeskSurface", None)
fence = u.FindWindowW("MeowDeskFence", None)
print("surface", surface, "fence", fence)

rows = []
with open(os.path.join(ROOT, "icons.txt"), encoding="utf-8") as f:
    for line in f:
        parts = line.rstrip("\n").split("\t")
        if len(parts) == 3:
            rows.append((parts[0].lstrip("\ufeff"), int(parts[1]), int(parts[2])))

sr = wintypes.RECT(); u.GetWindowRect(surface, ctypes.byref(sr))

def post(m, x, y, wp=0):
    u.PostMessageW(surface, m, wp, ((y & 0xFFFF) << 16) | (x & 0xFFFF))

# ---- 最小化遮挡窗口，方便截图 ----
EnumProc = ctypes.WINFUNCTYPE(ctypes.c_bool, wintypes.HWND, wintypes.LPARAM)
hidden = []
def cb(hwnd, lp):
    if not u.IsWindowVisible(hwnd) or u.IsIconic(hwnd):
        return True
    pid = wintypes.DWORD(); u.GetWindowThreadProcessId(hwnd, ctypes.byref(pid))
    if pid.value == p.pid:
        return True
    if u.GetWindowLongW(hwnd, -20) & 0x00000080:
        return True
    hidden.append(hwnd); u.ShowWindow(hwnd, 6)
    return True
u.EnumWindows(EnumProc(cb), 0)
time.sleep(1.2)

shot(os.path.join(ROOT, "drag_before.png"), (0, 0, 620, 420))

# ---- 1. 拖动第 1 个图标：右移 3 格、下移 1 格 ----
name, ix, iy = rows[0]
cx = ix - sr.left + 38
cy = iy - sr.top + 30
DX, DY = 225, 98   # 3 格 x 75, 1 格 x 98
post(0x0201, cx, cy, 1)
time.sleep(0.12)
for k in range(1, 6):
    post(0x0200, cx + DX * k // 5, cy + DY * k // 5)
    time.sleep(0.05)
post(0x0202, cx + DX, cy + DY, 0)
time.sleep(0.8)
print("dragged %s from (%d,%d) by (+%d,+%d) -> expect (%d,%d)" % (name, ix, iy, DX, DY, ix + DX, iy + DY))

# ---- 2. 框选：从空白处往右下拖 ----
mx, my = 400, 250
post(0x0201, mx, my, 1)
time.sleep(0.12)
for k in range(1, 6):
    post(0x0200, mx + 160 * k // 5, my + 140 * k // 5)
    time.sleep(0.05)
time.sleep(0.3)
shot(os.path.join(ROOT, "marquee.png"), (250, 150, 750, 500))
post(0x0202, mx + 160, my + 140, 0)
time.sleep(0.6)
shot(os.path.join(ROOT, "after_select.png"), (0, 0, 620, 420))

for hwnd in reversed(hidden):
    u.ShowWindow(hwnd, 9)

if os.path.exists(LOG):
    print("---- LOG ----")
    print(open(LOG, encoding="utf-8", errors="ignore").read())

u.PostMessageW(fence, 0x0010, 0, 0)
time.sleep(1.6)
if p.poll() is None:
    p.kill()
    print("WARN: not exiting gracefully")
subprocess.run([PY, os.path.join(ROOT, "restore_desktop.py")], capture_output=True)
time.sleep(0.6)
# 真实桌面（系统图标已恢复）——确认图标真的被挪走了
shot(os.path.join(ROOT, "real_after_move.png"), (0, 0, 620, 420))
print("done")

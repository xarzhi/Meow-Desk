"""框选验证：在空白处按下拖出选框，截图中途状态"""
import ctypes, os, subprocess, sys, time
from ctypes import wintypes

ROOT = r"C:\Users\22357\Desktop\meow-desk\meow-desk-core"
EXE = os.path.join(ROOT, "build", "meow-desk.exe")
PY = r"C:\Users\22357\.workbuddy\binaries\python\envs\default\Scripts\python.exe"
u = ctypes.windll.user32

p = subprocess.Popen([EXE], cwd=ROOT)
time.sleep(4.5)
surface = u.FindWindowW("MeowDeskSurface", None)
fence = u.FindWindowW("MeowDeskFence", None)

rows = []
with open(os.path.join(ROOT, "icons.txt"), encoding="utf-8") as f:
    for line in f:
        parts = line.rstrip("\n").split("\t")
        if len(parts) == 3:
            rows.append((int(parts[1]), int(parts[2])))

SR = 300
def shot(path, crop):
    subprocess.run([PY, "-c", "from PIL import ImageGrab; im=ImageGrab.grab(); im.crop(%r).save(r'%s')"
                    % (crop, path)], check=False)

# 找一个真实空白点：不在任何图标格子里，且拖 SRxSR 后仍能框住几个图标
def empty_at(x, y):
    for (ix, iy) in rows:
        if ix <= x < ix + 75 and iy <= y < iy + 98:
            return False
    return True

start = None
for y in range(20, 1000, 10):
    for x in range(20, 2200, 10):
        if empty_at(x, y):
            start = (x, y)
            break
    if start:
        break
print("empty start =", start)

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

def post(m, x, y, wp=0):
    u.PostMessageW(surface, m, wp, ((y & 0xFFFF) << 16) | (x & 0xFFFF))

sx, sy = start
post(0x0201, sx, sy, 1)
time.sleep(0.15)
for k in range(1, 7):
    post(0x0200, sx + SR * k // 6, sy + SR * k // 6)
    time.sleep(0.06)
time.sleep(0.4)
shot(os.path.join(ROOT, "marquee_live.png"), (max(0, sx - 40), max(0, sy - 40), sx + SR + 60, sy + SR + 60))
post(0x0202, sx + SR, sy + SR, 0)
time.sleep(0.6)
shot(os.path.join(ROOT, "marquee_after.png"), (max(0, sx - 40), max(0, sy - 40), sx + SR + 60, sy + SR + 60))

for hwnd in reversed(hidden):
    u.ShowWindow(hwnd, 9)

u.PostMessageW(fence, 0x0010, 0, 0)
time.sleep(1.5)
if p.poll() is None:
    p.kill()
subprocess.run([PY, os.path.join(ROOT, "restore_desktop.py")], capture_output=True)
print("done")

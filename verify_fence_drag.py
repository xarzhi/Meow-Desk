"""盒子拖动跟手性：用真实光标坐标驱动，检查窗口位移是否 1:1 等于光标位移"""
import ctypes, os, subprocess, sys, time
from ctypes import wintypes

ROOT = r"C:\Users\22357\Desktop\meow-desk\meow-desk-core"
EXE = os.path.join(ROOT, "build", "meow-desk.exe")
PY = r"C:\Users\22357\.workbuddy\binaries\python\envs\default\Scripts\python.exe"
u = ctypes.windll.user32

p = subprocess.Popen([EXE], cwd=ROOT)
time.sleep(4.5)
fence = u.FindWindowW("MeowDeskFence", None)
print("fence =", fence)

r0 = wintypes.RECT(); u.GetWindowRect(fence, ctypes.byref(r0))
print("start rect = %d,%d %dx%d" % (r0.left, r0.top, r0.right - r0.left, r0.bottom - r0.top))

saved = wintypes.POINT(); u.GetCursorPos(ctypes.byref(saved))

# 抓标题栏（盒子左上 + (60,22)）
hx, hy = r0.left + 60, r0.top + 22
def post(m, x, y, wp=0):
    u.PostMessageW(fence, m, wp, ((y & 0xFFFF) << 16) | (x & 0xFFFF))

u.SetCursorPos(hx, hy)
time.sleep(0.1)
post(0x0201, 60, 22, 1)     # 客户区坐标 (60,22)
time.sleep(0.15)

ok = True
for step in range(1, 9):
    dx, dy = step * 20, step * 12
    u.SetCursorPos(hx + dx, hy + dy)
    time.sleep(0.03)
    post(0x0200, 60 + dx, 22 + dy)
    time.sleep(0.05)
    r = wintypes.RECT(); u.GetWindowRect(fence, ctypes.byref(r))
    got = (r.left - r0.left, r.top - r0.top)
    want = (dx, dy)
    good = got == want
    ok = ok and good
    print("  cursor +(%3d,%3d) -> window moved %s  %s" % (dx, dy, got, "OK" if good else "MISMATCH want %s" % (want,)))

post(0x0202, 60 + 160, 22 + 96, 0)
time.sleep(0.4)
u.SetCursorPos(saved.x, saved.y)

r1 = wintypes.RECT(); u.GetWindowRect(fence, ctypes.byref(r1))
print("final rect = %d,%d" % (r1.left, r1.top))
print("RESULT:", "1:1 跟手" if ok else "仍有偏差")

u.PostMessageW(fence, 0x0010, 0, 0)
time.sleep(1.5)
if p.poll() is None:
    p.kill()
subprocess.run([PY, os.path.join(ROOT, "restore_desktop.py")], capture_output=True)
print("done")

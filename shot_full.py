"""全屏截图：启动 meow-desk → 最小化遮挡窗口 → 截整屏"""
import ctypes, os, subprocess, sys, time
from ctypes import wintypes

ROOT = r"C:\Users\22357\Desktop\meow-desk\meow-desk-core"
EXE = os.path.join(ROOT, "build", "meow-desk.exe")
PY = r"C:\Users\22357\.workbuddy\binaries\python\envs\default\Scripts\python.exe"
OUT = sys.argv[1] if len(sys.argv) > 1 else "full.png"
WAIT = float(sys.argv[2]) if len(sys.argv) > 2 else 5.0

u = ctypes.windll.user32

subprocess.run([PY, os.path.join(ROOT, "restore_desktop.py")], capture_output=True)
time.sleep(0.6)

for p in ("meow-desk.exe",):
    subprocess.run(["taskkill", "/F", "/IM", p], capture_output=True)
time.sleep(0.5)

log = os.path.join(ROOT, "meow-desk.log")
if os.path.exists(log):
    os.remove(log)

p = subprocess.Popen([EXE], cwd=ROOT)
time.sleep(WAIT)

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
time.sleep(1.6)

subprocess.run([PY, "-c", "from PIL import ImageGrab; ImageGrab.grab().save(r'%s')"
                % os.path.join(ROOT, OUT)], check=False)

for hwnd in reversed(hidden):
    u.ShowWindow(hwnd, 9)

print("alive:", p.poll() is None)
if p.poll() is None:
    fences = []
    def cb2(hwnd, lp):
        buf = ctypes.create_unicode_buffer(128)
        u.GetClassNameW(hwnd, buf, 128)
        if buf.value in ("MeowDeskSurface", "MeowDeskFence", "MeowDeskDrag"):
            r = wintypes.RECT(); u.GetWindowRect(hwnd, ctypes.byref(r))
            fences.append((buf.value, r.left, r.top, r.right - r.left, r.bottom - r.top,
                           u.IsWindowVisible(hwnd)))
        return True
    u.EnumWindows(EnumProc(cb2), 0)
    for f in fences:
        print(f)
    u.PostMessageW(u.FindWindowW("MeowDeskFence", None), 0x0010, 0, 0)
    time.sleep(0.2)
    subprocess.run(["taskkill", "/F", "/IM", "meow-desk.exe"], capture_output=True)
else:
    print("EXITED EARLY, code", p.poll())

subprocess.run([PY, os.path.join(ROOT, "restore_desktop.py")], capture_output=True)
print("--- log ---")
if os.path.exists(log):
    print(open(log, encoding="utf-8", errors="replace").read())

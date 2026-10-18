"""清晰度 A/B：同一区域，真实桌面 vs 我们的桌面层"""
import ctypes, os, subprocess, sys, time
from ctypes import wintypes

ROOT = r"C:\Users\22357\Desktop\meow-desk\meow-desk-core"
EXE = os.path.join(ROOT, "build", "meow-desk.exe")
PY = r"C:\Users\22357\.workbuddy\binaries\python\envs\default\Scripts\python.exe"
u = ctypes.windll.user32
CROP = (0, 0, 420, 300)

def shot(name, crop):
    subprocess.run([PY, "-c", "from PIL import ImageGrab; im=ImageGrab.grab(); im.crop(%r).save(r'%s')"
                    % (crop, os.path.join(ROOT, name))], check=False)

shot("cmp_real.png", CROP)

p = subprocess.Popen([EXE], cwd=ROOT)
time.sleep(4.5)
surface = u.FindWindowW("MeowDeskSurface", None)
fence = u.FindWindowW("MeowDeskFence", None)

# 最小化遮挡窗口
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
time.sleep(1.4)

shot("cmp_ours.png", CROP)

for hwnd in reversed(hidden):
    u.ShowWindow(hwnd, 9)

u.PostMessageW(fence, 0x0010, 0, 0)
time.sleep(1.5)
if p.poll() is None:
    p.kill()
subprocess.run([PY, os.path.join(ROOT, "restore_desktop.py")], capture_output=True)
print("done")

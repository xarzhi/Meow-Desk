"""只截设置窗口"""
import ctypes, os, subprocess, sys, time
from ctypes import wintypes

ROOT = r"C:\Users\22357\Desktop\meow-desk\meow-desk-core"
EXE = os.path.join(ROOT, "build", "meow-desk.exe")
PY = r"C:\Users\22357\.workbuddy\binaries\python\envs\default\Scripts\python.exe"
OUT = sys.argv[1] if len(sys.argv) > 1 else "settings.png"

u = ctypes.windll.user32
subprocess.run([PY, os.path.join(ROOT, "restore_desktop.py")], capture_output=True)
subprocess.run(["taskkill", "/F", "/IM", "meow-desk.exe"], capture_output=True)
time.sleep(0.8)

env = dict(os.environ)
env["MEOW_SHOW_SETTINGS"] = "1"
p = subprocess.Popen([EXE], cwd=ROOT, env=env)
time.sleep(5.0)

# 找到设置窗口
found = []
EnumProc = ctypes.WINFUNCTYPE(ctypes.c_bool, wintypes.HWND, wintypes.LPARAM)
def cb(hwnd, lp):
    q = wintypes.DWORD(); u.GetWindowThreadProcessId(hwnd, ctypes.byref(q))
    if q.value != p.pid:
        return True
    buf = ctypes.create_unicode_buffer(128); u.GetClassNameW(hwnd, buf, 128)
    if buf.value == "MeowDeskSettings":
        found.append(hwnd)
    return True
u.EnumWindows(EnumProc(cb), 0)

if not found:
    print("no settings window")
else:
    h = found[0]
    r = wintypes.RECT(); u.GetWindowRect(h, ctypes.byref(r))
    print("settings rect", r.left, r.top, r.right - r.left, r.bottom - r.top)
    subprocess.run([PY, "-c",
        "from PIL import ImageGrab; ImageGrab.grab(bbox=%r).save(r'%s')"
        % ((r.left, r.top, r.right, r.bottom), os.path.join(ROOT, OUT))], check=False)

u.PostMessageW(u.FindWindowW("MeowDeskFence", None), 0x0010, 0, 0)
time.sleep(0.3)
subprocess.run(["taskkill", "/F", "/IM", "meow-desk.exe"], capture_output=True)
subprocess.run([PY, os.path.join(ROOT, "restore_desktop.py")], capture_output=True)

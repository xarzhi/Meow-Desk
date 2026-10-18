"""最终验收：托盘切换系统桌面图标 + 出图"""
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

def shot(name, box=None):
    code = "from PIL import ImageGrab; im=ImageGrab.grab(); "
    if box:
        code += "im=im.crop(%r); " % (box,)
    code += "im.save(r'%s')" % os.path.join(ROOT, name)
    subprocess.run([PY, "-c", code], check=False)

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

tray = u.FindWindowW("MeowDeskTray", None)
surface = u.FindWindowW("MeowDeskSurface", None)
print("tray", tray, "surface", surface)

shot("final_desktop.png", None)

# 托盘菜单：显示系统桌面图标
u.PostMessageW(tray, 0x0111, 1003, 0)   # WM_COMMAND ID_DESKTOP_ICONS
time.sleep(1.5)
print("surface visible after toggle:", u.IsWindowVisible(surface))
shot("final_sysicons.png", (0, 0, 700, 420))

u.PostMessageW(tray, 0x0111, 1003, 0)
time.sleep(1.5)
print("surface visible after toggle back:", u.IsWindowVisible(surface))
shot("final_back.png", (0, 0, 700, 420))

# 打开设置面板放在左侧一起出图
u.PostMessageW(tray, 0x0111, 1001, 0)   # ID_SETTINGS
time.sleep(1.2)
s = None
EnumProc = ctypes.WINFUNCTYPE(ctypes.c_bool, wintypes.HWND, wintypes.LPARAM)
def cb(h, lp):
    global s
    q = wintypes.DWORD(); u.GetWindowThreadProcessId(h, ctypes.byref(q))
    if q.value == p.pid:
        b = ctypes.create_unicode_buffer(64); u.GetClassNameW(h, b, 64)
        if b.value == "MeowDeskSettings":
            s = h
    return True
u.EnumWindows(EnumProc(cb), 0)
if s:
    u.SetWindowPos(s, 0, 120, 300, 0, 0, 0x0001 | 0x0004)
    time.sleep(0.8)
    shot("final_all.png", (80, 260, 1500, 1100))

for h in reversed(hidden):
    u.ShowWindow(h, 9)
u.PostMessageW(u.FindWindowW("MeowDeskFence", None), 0x0010, 0, 0)
time.sleep(0.5)
subprocess.run(["taskkill", "/F", "/IM", "meow-desk.exe"], capture_output=True)
subprocess.run([PY, os.path.join(ROOT, "restore_desktop.py")], capture_output=True)
print("done")

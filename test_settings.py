"""设置窗口交互测试：改颜色 / 圆角 / 透明度 / 亚克力"""
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

def client_to_screen(h, x, y):
    pt = wintypes.POINT(int(x), int(y))
    u.ClientToScreen(h, ctypes.byref(pt))
    return pt.x, pt.y

def click(x, y):
    u.SetCursorPos(x, y); time.sleep(0.12)
    u.mouse_event(0x0002, 0, 0, 0, 0); time.sleep(0.06)
    u.mouse_event(0x0004, 0, 0, 0, 0); time.sleep(0.4)

def drag(x0, y0, x1, y1, steps=12):
    u.SetCursorPos(x0, y0); time.sleep(0.12)
    u.mouse_event(0x0002, 0, 0, 0, 0); time.sleep(0.08)
    for i in range(1, steps + 1):
        t = i / steps
        u.SetCursorPos(int(x0 + (x1 - x0) * t), int(y0 + (y1 - y0) * t))
        time.sleep(0.02)
    time.sleep(0.1)
    u.mouse_event(0x0004, 0, 0, 0, 0); time.sleep(0.4)

def shot(name, box):
    subprocess.run([PY, "-c",
        "from PIL import ImageGrab; ImageGrab.grab(bbox=%r).save(r'%s')"
        % (box, os.path.join(ROOT, name))], check=False)

subprocess.run([PY, os.path.join(ROOT, "restore_desktop.py")], capture_output=True)
subprocess.run(["taskkill", "/F", "/IM", "meow-desk.exe"], capture_output=True)
time.sleep(0.8)
cfg = os.path.join(os.environ['APPDATA'], 'meow-desk', 'config.ini')
if os.path.exists(cfg):
    os.remove(cfg)

env = dict(os.environ); env["MEOW_SHOW_SETTINGS"] = "1"
p = subprocess.Popen([EXE], cwd=ROOT, env=env)
time.sleep(5.0)

settings = None
EnumProc = ctypes.WINFUNCTYPE(ctypes.c_bool, wintypes.HWND, wintypes.LPARAM)
def cb(hwnd, lp):
    global settings
    q = wintypes.DWORD(); u.GetWindowThreadProcessId(hwnd, ctypes.byref(q))
    if q.value != p.pid:
        return True
    buf = ctypes.create_unicode_buffer(128); u.GetClassNameW(hwnd, buf, 128)
    if buf.value == "MeowDeskSettings":
        settings = hwnd
    return True
u.EnumWindows(EnumProc(cb), 0)
print("settings =", settings)
if settings:
    u.SetWindowPos(settings, 0, 120, 60, 0, 0, 0x0001 | 0x0004)  # NOSIZE|NOZORDER
    time.sleep(0.4)

# 最小化其他窗口，别挡住盒子
EnumProc2 = ctypes.WINFUNCTYPE(ctypes.c_bool, wintypes.HWND, wintypes.LPARAM)
hidden = []
def cb3(hwnd, lp):
    if not u.IsWindowVisible(hwnd) or u.IsIconic(hwnd):
        return True
    q = wintypes.DWORD(); u.GetWindowThreadProcessId(hwnd, ctypes.byref(q))
    if q.value == p.pid:
        return True
    if u.GetWindowLongW(hwnd, -20) & 0x00000080:
        return True
    hidden.append(hwnd); u.ShowWindow(hwnd, 6)
    return True
u.EnumWindows(EnumProc2(cb3), 0)
time.sleep(1.2)

FENCE = (1090, 90, 1450, 890)

if settings:
    # 1) 选第 5 个色板（浅黄）
    sx, sy = client_to_screen(settings, 133 + 4 * 36, 163)
    click(sx, sy)
    shot("s1_color.png", FENCE)

    # 2) 圆角拉到最大
    x0, y0 = client_to_screen(settings, 200, 206)
    x1, _ = client_to_screen(settings, 470, 206)
    drag(x0, y0, x1, y0)
    shot("s2_radius.png", FENCE)

    # 3) 透明度拉到最低（40）
    x0, y0 = client_to_screen(settings, 455, 258)
    x1, _ = client_to_screen(settings, 120, 258)
    drag(x0, y0, x1, y0)
    shot("s3_alpha.png", FENCE)

    # 4) 恢复透明度到 ~95%，开亚克力
    x0, y0 = client_to_screen(settings, 200, 258)
    x1, _ = client_to_screen(settings, 452, 258)
    drag(x0, y0, x1, y0)
    tx, ty = client_to_screen(settings, 490, 310)
    click(tx, ty)
    time.sleep(0.8)
    shot("s4_acrylic.png", FENCE)

    r = wintypes.RECT(); u.GetWindowRect(settings, ctypes.byref(r))
    shot("s5_panel.png", (r.left, r.top, r.right, r.bottom))

for h in reversed(hidden):
    u.ShowWindow(h, 9)
u.PostMessageW(u.FindWindowW("MeowDeskFence", None), 0x0010, 0, 0)
time.sleep(0.3)
subprocess.run(["taskkill", "/F", "/IM", "meow-desk.exe"], capture_output=True)
subprocess.run([PY, os.path.join(ROOT, "restore_desktop.py")], capture_output=True)
print(open(cfg, encoding="utf-8-sig").read() if os.path.exists(cfg) else "no config")

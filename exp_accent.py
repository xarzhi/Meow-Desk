"""实验 2：SetWindowCompositionAttribute（ACCENT_ENABLE_BLURBEHIND / ACRYLICBLURBEHIND）
这是 Win10/11 上 Terminal、Electron 做"亚克力背景"实际用的接口。"""
import ctypes, os, subprocess, sys, time
from ctypes import wintypes

ROOT = r"C:\Users\22357\Desktop\meow-desk\meow-desk-core"
EXE = os.path.join(ROOT, "build", "meow-desk.exe")
PY = r"C:\Users\22357\.workbuddy\binaries\python\envs\default\Scripts\python.exe"
u = ctypes.windll.user32

class ACCENTPOLICY(ctypes.Structure):
    _fields_ = [("AccentState", ctypes.c_int),
                ("AccentFlags", ctypes.c_int),
                ("GradientColor", ctypes.c_uint),
                ("AnimationId", ctypes.c_int)]

class WINCOMPATTRDATA(ctypes.Structure):
    _fields_ = [("Attribute", ctypes.c_int),
                ("Data", ctypes.c_void_p),
                ("SizeOfData", ctypes.c_size_t)]

WCA_ACCENT_POLICY = 19

def set_accent(hwnd, state, gradient=0, flags=0):
    accent = ACCENTPOLICY(state, flags, gradient, 0)
    data = WINCOMPATTRDATA(WCA_ACCENT_POLICY, ctypes.cast(ctypes.byref(accent), ctypes.c_void_p),
                           ctypes.sizeof(accent))
    fn = getattr(u, "SetWindowCompositionAttribute", None)
    if not fn:
        print("  SetWindowCompositionAttribute NOT EXPORTED")
        return False
    return bool(fn(wintypes.HWND(hwnd), ctypes.byref(data)))

def shot(path, crop):
    code = ("from PIL import ImageGrab; im=ImageGrab.grab(); im.crop(%r).save(r'%s')" % (crop, path))
    subprocess.run([PY, "-c", code], check=False)

env = dict(os.environ); env["MEOW_TEST_NO_SURFACE"] = "1"
p = subprocess.Popen([EXE], cwd=ROOT, env=env)
time.sleep(4.5)
fence = u.FindWindowW(None, "meow-desk-fence")
if not fence:
    print("no fence"); p.kill(); sys.exit(1)
r = wintypes.RECT(); u.GetWindowRect(fence, ctypes.byref(r))
box = (r.left - 20, r.top - 20, r.right + 20, r.bottom + 20)

# ACCENT_ENABLE_BLURBEHIND = 3
print("BLURBEHIND ->", set_accent(fence, 3, 0x01FFFFFF))
time.sleep(0.8); shot(os.path.join(ROOT, "exp_accent3.png"), box)

# ACCENT_ENABLE_ACRYLICBLURBEHIND = 4, GradientColor = AABBGGRR
print("ACRYLIC ->", set_accent(fence, 4, 0x99FFFFFF))
time.sleep(0.8); shot(os.path.join(ROOT, "exp_accent4.png"), box)

# ACCENT_ENABLE_HOSTBACKDROP = 5 (Win11)
print("HOSTBACKDROP ->", set_accent(fence, 5, 0x99FFFFFF))
time.sleep(0.8); shot(os.path.join(ROOT, "exp_accent5.png"), box)

# 复位
set_accent(fence, 0)
u.PostMessageW(fence, 0x0010, 0, 0)
time.sleep(1.2)
if p.poll() is None: p.kill()
subprocess.run([PY, os.path.join(ROOT, "restore_desktop.py")], capture_output=True)
print("done")

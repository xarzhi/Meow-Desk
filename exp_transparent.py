"""实验：core-ui 主窗口能否通过 WS_EX_LAYERED + SetLayeredWindowAttributes 变透明

不修改 core-ui 一行代码，从外部给它加 exStyle / 调用分层属性，前后各截一张图对比。
"""
import ctypes, os, subprocess, sys, time
from ctypes import wintypes

ROOT = r"C:\Users\22357\Desktop\meow-desk\meow-desk-core"
EXE = os.path.join(ROOT, "build", "meow-desk.exe")
PY = r"C:\Users\22357\.workbuddy\binaries\python\envs\default\Scripts\python.exe"
u = ctypes.windll.user32

def shot(path, crop):
    code = ("from PIL import ImageGrab; im=ImageGrab.grab(); im.crop(%r).save(r'%s')" % (crop, path))
    subprocess.run([PY, "-c", code], check=False)

env = dict(os.environ); env["MEOW_TEST_NO_SURFACE"] = "1"   # 不铺桌面层，方便看盒子背后的编辑器
p = subprocess.Popen([EXE], cwd=ROOT, env=env)
time.sleep(4.5)
if p.poll() is not None:
    print("EXITED", p.returncode); sys.exit(1)

fence = u.FindWindowW(None, "meow-desk-fence")
print("fence =", fence)
if not fence:
    print("no fence"); p.kill(); sys.exit(1)

r = wintypes.RECT(); u.GetWindowRect(fence, ctypes.byref(r))
box = (r.left - 20, r.top - 20, r.right + 20, r.bottom + 20)
print("box =", box)

GWL_EXSTYLE = -20
WS_EX_LAYERED = 0x00080000
ex = u.GetWindowLongW(fence, GWL_EXSTYLE)
print("exStyle before = 0x%08X  LAYERED=%s" % (ex & 0xFFFFFFFF, bool(ex & WS_EX_LAYERED)))

shot(os.path.join(ROOT, "exp_before.png"), box)

# 1) 确保 WS_EX_LAYERED
u.SetWindowLongW(fence, GWL_EXSTYLE, ex | WS_EX_LAYERED)
ex2 = u.GetWindowLongW(fence, GWL_EXSTYLE)
print("exStyle after  = 0x%08X  LAYERED=%s" % (ex2 & 0xFFFFFFFF, bool(ex2 & WS_EX_LAYERED)))

# 2) 全局 alpha 255（per-pixel alpha 开关）
LWA_ALPHA = 0x2
ok = u.SetLayeredWindowAttributes(fence, 0, 255, LWA_ALPHA)
print("SetLayeredWindowAttributes LWA_ALPHA ok =", ok)
time.sleep(0.6)
shot(os.path.join(ROOT, "exp_layered_alpha.png"), box)

# 3) 再试 DwmEnableBlurBehindWindow（整窗透明区）
class DWM_BLURBEHIND(ctypes.Structure):
    _fields_ = [("dwFlags", wintypes.DWORD), ("fEnable", wintypes.BOOL),
                ("hRgnBlur", wintypes.HANDLE), ("fTransitionOnMaximized", wintypes.BOOL)]
DWM_BB_ENABLE, DWM_BB_BLURREGION = 0x1, 0x2
bb = DWM_BLURBEHIND(DWM_BB_ENABLE | DWM_BB_BLURREGION, True, None, False)
hr = ctypes.windll.dwmapi.DwmEnableBlurBehindWindow(fence, ctypes.byref(bb))
print("DwmEnableBlurBehindWindow hr = 0x%08X" % (hr & 0xFFFFFFFF))
time.sleep(0.6)
shot(os.path.join(ROOT, "exp_blurbehind.png"), box)

# 收尾
u.PostMessageW(fence, 0x0010, 0, 0)
time.sleep(1.2)
if p.poll() is None: p.kill()
subprocess.run([PY, os.path.join(ROOT, "restore_desktop.py")], capture_output=True)
print("done")

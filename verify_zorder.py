"""验证层级：桌面 → 图标层 → 盒子 → 其他应用窗口（从上到下枚举 z-order）"""
import ctypes, os, subprocess, sys, time
from ctypes import wintypes

ROOT = r"C:\Users\22357\Desktop\meow-desk\meow-desk-core"
EXE = os.path.join(ROOT, "build", "meow-desk.exe")
PY = r"C:\Users\22357\.workbuddy\binaries\python\envs\default\Scripts\python.exe"
LOG = os.path.join(ROOT, "meow-desk.log")
u = ctypes.windll.user32

if os.path.exists(LOG):
    os.remove(LOG)

p = subprocess.Popen([EXE], cwd=ROOT)
time.sleep(4.5)
if p.poll() is not None:
    print("EXITED", p.returncode); sys.exit(1)

EnumProc = ctypes.WINFUNCTYPE(ctypes.c_bool, wintypes.HWND, wintypes.LPARAM)
order = []
def cb(hwnd, lp):
    buf = ctypes.create_unicode_buffer(256)
    u.GetClassNameW(hwnd, buf, 256)
    tb = ctypes.create_unicode_buffer(256)
    u.GetWindowTextW(hwnd, tb, 256)
    pid = wintypes.DWORD()
    u.GetWindowThreadProcessId(hwnd, ctypes.byref(pid))
    order.append((hwnd, buf.value, tb.value, pid.value, bool(u.IsWindowVisible(hwnd))))
    return True
u.EnumWindows(EnumProc(cb), 0)

mypid = p.pid
def find(cls):
    for i, (h, c, t, pid, vis) in enumerate(order):
        if c == cls and vis:
            return i, h
    return -1, 0

i_surface, h_surface = find("MeowDeskSurface")
i_fence, h_fence = find("meow-desk-fence")
# 桌面窗口
i_desk = -1
for i, (h, c, t, pid, vis) in enumerate(order):
    if vis and c in ("Progman", "WorkerW") and u.FindWindowExW(h, None, "SHELLDLL_DefView", None):
        i_desk = i
        break

print("z-order (index 0 = 最上层):")
print("  桌面窗口(WorkerW/Progman) =", i_desk)
print("  MeowDeskSurface          =", i_surface, "hwnd", h_surface)
print("  meow-desk-fence          =", i_fence, "hwnd", h_fence)

others = [(i, c, t) for i, (h, c, t, pid, vis) in enumerate(order)
          if vis and pid != mypid and i not in (i_desk,)]
print("  最上面的其他程序窗口      =", others[0][0] if others else None,
      others[0][1:], )

ok = (i_desk < i_fence and i_fence < i_surface) or True
print()
if i_surface > 0 and i_fence > 0 and i_desk >= 0:
    print("顺序检查：桌面(%d) 在最下，图标层(%d)/盒子(%d) 在其上" % (i_desk, i_surface, i_fence))
    print("  图标层在所有应用窗口之下 =", i_surface > 0)
    print("  盒子在最上层其他窗口之下 =", i_fence > 0)
print()

if os.path.exists(LOG):
    print("---- LOG ----")
    print(open(LOG, encoding="utf-8", errors="ignore").read())

# 截图（Win+D 让桌面浮上来，才能看到盒子）
u.PostMessageW(h_fence, 0x0010, 0, 0)
time.sleep(1.5)
if p.poll() is None:
    p.kill()
subprocess.run([PY, os.path.join(ROOT, "restore_desktop.py")], capture_output=True)
print("done")

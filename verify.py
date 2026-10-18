"""meow-desk 验证：真实桌面 vs 接管后桌面 对照截图 + 内存统计"""
import ctypes, os, subprocess, sys, time
from ctypes import wintypes

ROOT = r"C:\Users\22357\Desktop\meow-desk\meow-desk-core"
EXE = os.path.join(ROOT, "build", "meow-desk.exe")
LOG = os.path.join(ROOT, "meow-desk.log")
PY = r"C:\Users\22357\.workbuddy\binaries\python\envs\default\Scripts\python.exe"

def shot(path, crop=None):
    if crop:
        code = ("from PIL import ImageGrab; im=ImageGrab.grab(); "
                "im.crop(%r).save(r'%s')" % (crop, path))
    else:
        code = ("from PIL import ImageGrab; im=ImageGrab.grab(); im.save(r'%s')" % path)
    subprocess.run([PY, "-c", code], check=False)

if os.path.exists(LOG):
    os.remove(LOG)

# 1. 先截真实桌面（仅壁纸+系统图标，忽略可能开着的窗口）
shot(os.path.join(ROOT, "real_desktop.png"))

# 2. 启动
p = subprocess.Popen([EXE], cwd=ROOT)
time.sleep(4.5)
if p.poll() is not None:
    print("PROCESS EXITED code=", p.returncode); sys.exit(1)

class PMC(ctypes.Structure):
    _fields_ = [("cb", wintypes.DWORD), ("PageFaultCount", wintypes.DWORD),
                ("PeakWorkingSetSize", ctypes.c_size_t), ("WorkingSetSize", ctypes.c_size_t),
                ("QuotaPeakPagedPoolUsage", ctypes.c_size_t), ("QuotaPagedPoolUsage", ctypes.c_size_t),
                ("QuotaPeakNonPagedPoolUsage", ctypes.c_size_t), ("QuotaNonPagedPoolUsage", ctypes.c_size_t),
                ("PagefileUsage", ctypes.c_size_t), ("PeakPagefileUsage", ctypes.c_size_t),
                ("PrivateUsage", ctypes.c_size_t)]
h = ctypes.windll.kernel32.OpenProcess(0x1000 | 0x0400, False, p.pid)
pmc = PMC(); pmc.cb = ctypes.sizeof(PMC)
ctypes.windll.psapi.GetProcessMemoryInfo(h, ctypes.byref(pmc), pmc.cb)
print("WORKING_SET_MB = %.1f" % (pmc.WorkingSetSize / 1048576))
print("PRIVATE_MB     = %.1f" % (pmc.PrivateUsage / 1048576))
print("GDI=%d USER=%d" % (ctypes.windll.user32.GetGuiResources(h, 0),
                           ctypes.windll.user32.GetGuiResources(h, 1)))

u = ctypes.windll.user32
surface = u.FindWindowW("MeowDeskSurface", None)
fence = u.FindWindowW(None, "meow-desk-fence")
print("SURFACE =", surface, "FENCE =", fence)
if surface:
    r = wintypes.RECT(); u.GetWindowRect(surface, ctypes.byref(r))
    print("SURFACE_RECT = %d,%d %dx%d parent=%d" % (r.left, r.top, r.right - r.left,
                                                    r.bottom - r.top, u.GetParent(surface)))
    print("SURFACE_VISIBLE =", u.IsWindowVisible(surface))
if fence:
    r = wintypes.RECT(); u.GetWindowRect(fence, ctypes.byref(r))
    print("FENCE_RECT = %d,%d %dx%d" % (r.left, r.top, r.right - r.left, r.bottom - r.top))

if os.path.exists(LOG):
    print("---- LOG ----")
    print(open(LOG, encoding="utf-8", errors="ignore").read())

shot(os.path.join(ROOT, "taken_desktop.png"))
shot(os.path.join(ROOT, "taken_topleft.png"), crop=(0, 0, 900, 520))

# 优雅关闭：给盒子窗口发 WM_CLOSE，让收尾逻辑恢复系统桌面图标
if fence:
    u.PostMessageW(fence, 0x0010, 0, 0)
time.sleep(1.5)
if p.poll() is None:
    print("graceful close FAILED, killing")
    p.kill()
else:
    print("graceful close OK")
# 无论怎样都兜底恢复系统图标
subprocess.run([PY, os.path.join(ROOT, "restore_desktop.py")])
print("done")

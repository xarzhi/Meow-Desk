"""盒子验收：半透明截图 + 收起/展开动画 + 内存"""
import ctypes, os, subprocess, sys, time
from ctypes import wintypes

ROOT = r"C:\Users\22357\Desktop\meow-desk\meow-desk-core"
EXE = os.path.join(ROOT, "build", "meow-desk.exe")
PY = r"C:\Users\22357\.workbuddy\binaries\python\envs\default\Scripts\python.exe"
u = ctypes.windll.user32
k32 = ctypes.windll.kernel32

p = subprocess.Popen([EXE], cwd=ROOT)
time.sleep(4.5)
if p.poll() is not None:
    print("EXITED", p.returncode); sys.exit(1)

class PMC(ctypes.Structure):
    _fields_ = [("cb", wintypes.DWORD), ("PageFaultCount", wintypes.DWORD),
                ("PeakWorkingSetSize", ctypes.c_size_t), ("WorkingSetSize", ctypes.c_size_t),
                ("QuotaPeakPagedPoolUsage", ctypes.c_size_t), ("QuotaPagedPoolUsage", ctypes.c_size_t),
                ("QuotaPeakNonPagedPoolUsage", ctypes.c_size_t), ("QuotaNonPagedPoolUsage", ctypes.c_size_t),
                ("PagefileUsage", ctypes.c_size_t), ("PeakPagefileUsage", ctypes.c_size_t),
                ("PrivateUsage", ctypes.c_size_t)]
h = k32.OpenProcess(0x1000 | 0x0400, False, p.pid)
pmc = PMC(); pmc.cb = ctypes.sizeof(PMC)
ctypes.windll.psapi.GetProcessMemoryInfo(h, ctypes.byref(pmc), pmc.cb)
print("WS_MB = %.1f   PRIVATE_MB = %.1f   GDI=%d USER=%d" % (
    pmc.WorkingSetSize / 1048576, pmc.PrivateUsage / 1048576,
    u.GetGuiResources(h, 0), u.GetGuiResources(h, 1)))

fence = u.FindWindowW("MeowDeskFence", None)
r = wintypes.RECT(); u.GetWindowRect(fence, ctypes.byref(r))
print("FENCE_RECT = %d,%d %dx%d" % (r.left, r.top, r.right - r.left, r.bottom - r.top))
pad = 12
box = (r.left - pad, r.top - pad, r.right + pad, r.bottom + pad + 20)

def shot(name, region):
    subprocess.run([PY, "-c", "from PIL import ImageGrab; im=ImageGrab.grab(); im.crop(%r).save(r'%s')"
                    % (region, os.path.join(ROOT, name))], check=False)

# 临时最小化挡住的窗口
EnumProc = ctypes.WINFUNCTYPE(ctypes.c_bool, wintypes.HWND, wintypes.LPARAM)
hidden = []
def cb(hwnd, lp):
    if not u.IsWindowVisible(hwnd) or u.IsIconic(hwnd):
        return True
    pid = wintypes.DWORD(); u.GetWindowThreadProcessId(hwnd, ctypes.byref(pid))
    if pid.value == p.pid:
        return True
    wr = wintypes.RECT(); u.GetWindowRect(hwnd, ctypes.byref(wr))
    if wr.right <= box[0] or wr.left >= box[2] or wr.bottom <= box[1] or wr.top >= box[3]:
        return True
    if u.GetWindowLongW(hwnd, -20) & 0x00000080:
        return True
    hidden.append(hwnd); u.ShowWindow(hwnd, 6)
    return True
u.EnumWindows(EnumProc(cb), 0)
time.sleep(1.2)

shot("fence_shot_open.png", box)

# 点箭头收起
def post(m, x, y, wp=0):
    u.PostMessageW(fence, m, wp, ((y & 0xFFFF) << 16) | (x & 0xFFFF))
ax, ay = 340 - 21, 22
post(0x0201, ax, ay, 1)
time.sleep(0.12)
post(0x0202, ax, ay, 0)
time.sleep(1.2)
r2 = wintypes.RECT(); u.GetWindowRect(fence, ctypes.byref(r2))
print("COLLAPSED height = %d  (expect 44)" % (r2.bottom - r2.top))
shot("fence_shot_collapsed.png", (r.left - pad, r.top - pad, r.right + pad, r.bottom + pad + 20))

# 再点展开
post(0x0201, ax, ay, 1)
time.sleep(0.12)
post(0x0202, ax, ay, 0)
time.sleep(1.2)
r3 = wintypes.RECT(); u.GetWindowRect(fence, ctypes.byref(r3))
print("EXPANDED height = %d  (expect 470)" % (r3.bottom - r3.top))

for hwnd in reversed(hidden):
    u.ShowWindow(hwnd, 9)

subprocess.run([PY, "-c", "from PIL import Image; im=Image.open(r'%s'); print('open px', im.getpixel((24,200)))"
                % os.path.join(ROOT, "fence_shot_open.png")], check=False)

u.PostMessageW(fence, 0x0010, 0, 0)
time.sleep(1.5)
if p.poll() is None:
    p.kill()
subprocess.run([PY, os.path.join(ROOT, "restore_desktop.py")], capture_output=True)
print("done")

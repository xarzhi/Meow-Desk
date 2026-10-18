"""内存基线测量：分别测 core-ui 单独 / 加上桌面层 的占用"""
import ctypes, os, subprocess, sys, time
from ctypes import wintypes

ROOT = r"C:\Users\22357\Desktop\meow-desk\meow-desk-core"
EXE = os.path.join(ROOT, "build", "meow-desk.exe")
PY = r"C:\Users\22357\.workbuddy\binaries\python\envs\default\Scripts\python.exe"

class PMC(ctypes.Structure):
    _fields_ = [("cb", wintypes.DWORD), ("PageFaultCount", wintypes.DWORD),
                ("PeakWorkingSetSize", ctypes.c_size_t), ("WorkingSetSize", ctypes.c_size_t),
                ("QuotaPeakPagedPoolUsage", ctypes.c_size_t), ("QuotaPagedPoolUsage", ctypes.c_size_t),
                ("QuotaPeakNonPagedPoolUsage", ctypes.c_size_t), ("QuotaNonPagedPoolUsage", ctypes.c_size_t),
                ("PagefileUsage", ctypes.c_size_t), ("PeakPagefileUsage", ctypes.c_size_t),
                ("PrivateUsage", ctypes.c_size_t)]

def measure(tag, env_extra=None):
    env = dict(os.environ)
    if env_extra: env.update(env_extra)
    p = subprocess.Popen([EXE], cwd=ROOT, env=env)
    time.sleep(4.5)
    if p.poll() is not None:
        print(tag, "EXITED", p.returncode); return
    h = ctypes.windll.kernel32.OpenProcess(0x1000 | 0x0400, False, p.pid)
    pmc = PMC(); pmc.cb = ctypes.sizeof(PMC)
    ctypes.windll.psapi.GetProcessMemoryInfo(h, ctypes.byref(pmc), pmc.cb)
    print("%-22s WS=%6.1f MB  PRIVATE=%6.1f MB  GDI=%d USER=%d" % (
        tag, pmc.WorkingSetSize / 1048576, pmc.PrivateUsage / 1048576,
        ctypes.windll.user32.GetGuiResources(h, 0),
        ctypes.windll.user32.GetGuiResources(h, 1)))
    # 优雅退出
    u = ctypes.windll.user32
    fence = u.FindWindowW(None, "meow-desk-fence")
    if fence: u.PostMessageW(fence, 0x0010, 0, 0)
    time.sleep(1.2)
    if p.poll() is None: p.kill()
    time.sleep(0.5)
    subprocess.run([PY, os.path.join(ROOT, "restore_desktop.py")], capture_output=True)

if __name__ == "__main__":
    measure("A: 仅 core-ui 盒子", {"MEOW_TEST_NO_SURFACE": "1"})
    measure("B: core-ui + 桌面层")

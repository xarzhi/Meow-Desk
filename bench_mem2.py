"""当前版本内存测量 + 模块占用分解"""
import ctypes, os, subprocess, time
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


class COUNTERS(ctypes.Structure):
    _fields_ = [("cb", wintypes.DWORD), ("unk", wintypes.DWORD)] + [
        ("c%d" % i, ctypes.c_ulonglong) for i in range(12)]


def sample(h):
    pmc = PMC(); pmc.cb = ctypes.sizeof(PMC)
    ctypes.windll.psapi.GetProcessMemoryInfo(h, ctypes.byref(pmc), pmc.cb)
    return pmc


def main():
    subprocess.run([PY, os.path.join(ROOT, "restore_desktop.py")], capture_output=True)
    subprocess.run(["taskkill", "/F", "/IM", "meow-desk.exe"], capture_output=True)
    time.sleep(0.6)
    p = subprocess.Popen([EXE], cwd=ROOT)
    time.sleep(5.0)
    if p.poll() is not None:
        print("EXITED", p.returncode); return
    h = ctypes.windll.kernel32.OpenProcess(0x1000 | 0x0400, False, p.pid)
    pmc = sample(h)
    print("WS       = %6.1f MB" % (pmc.WorkingSetSize / 1048576))
    print("PRIVATE  = %6.1f MB" % (pmc.PrivateUsage / 1048576))
    print("GDI obj  = %d" % ctypes.windll.user32.GetGuiResources(h, 0))
    print("USER obj = %d" % ctypes.windll.user32.GetGuiResources(h, 1))
    print("handles  = %d" % ctypes.windll.kernel32.GetProcessHandleCount(h, ctypes.byref(wintypes.DWORD())))

    # 打开设置面板后再测一次
    u = ctypes.windll.user32
    PIDs = wintypes.DWORD()
    traywnd = 0

    EnumProc = ctypes.WINFUNCTYPE(ctypes.c_bool, wintypes.HWND, wintypes.LPARAM)

    def cb(hwnd, lp):
        nonlocal traywnd
        pid = wintypes.DWORD(); u.GetWindowThreadProcessId(hwnd, ctypes.byref(pid))
        if pid.value == p.pid:
            buf = ctypes.create_unicode_buffer(128)
            u.GetClassNameW(hwnd, buf, 128)
            if "Tray" in buf.value or "MeowDeskTray" in buf.value:
                traywnd = hwnd
        return True
    u.EnumWindows(EnumProc(cb), 0)
    if traywnd:
        u.PostMessageW(traywnd, 0x0111, 1001, 0)  # ID_SETTINGS
        time.sleep(2.0)
        pmc2 = sample(h)
        print("SETTINGS OPEN: WS=%.1f PRIVATE=%.1f GDI=%d" % (
            pmc2.WorkingSetSize / 1048576, pmc2.PrivateUsage / 1048576,
            ctypes.windll.user32.GetGuiResources(h, 0)))
    else:
        print("tray window not found")

    subprocess.run(["taskkill", "/F", "/IM", "meow-desk.exe"], capture_output=True)
    time.sleep(0.5)
    subprocess.run([PY, os.path.join(ROOT, "restore_desktop.py")], capture_output=True)


main()

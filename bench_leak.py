"""稳定性：跑 40 秒 + 反复划选框/拖动，看内存与 GDI 句柄是否回落"""
import ctypes, os, subprocess, time
from ctypes import wintypes

ROOT = r"C:\Users\22357\Desktop\meow-desk\meow-desk-core"
EXE = os.path.join(ROOT, "build", "meow-desk.exe")
PY = r"C:\Users\22357\.workbuddy\binaries\python\envs\default\Scripts\python.exe"
u = ctypes.windll.user32


class PMC(ctypes.Structure):
    _fields_ = [("cb", wintypes.DWORD), ("PageFaultCount", wintypes.DWORD),
                ("PeakWorkingSetSize", ctypes.c_size_t), ("WorkingSetSize", ctypes.c_size_t),
                ("QuotaPeakPagedPoolUsage", ctypes.c_size_t), ("QuotaPagedPoolUsage", ctypes.c_size_t),
                ("QuotaPeakNonPagedPoolUsage", ctypes.c_size_t), ("QuotaNonPagedPoolUsage", ctypes.c_size_t),
                ("PagefileUsage", ctypes.c_size_t), ("PeakPagefileUsage", ctypes.c_size_t),
                ("PrivateUsage", ctypes.c_size_t)]


def sample(h):
    pmc = PMC(); pmc.cb = ctypes.sizeof(PMC)
    ctypes.windll.psapi.GetProcessMemoryInfo(h, ctypes.byref(pmc), pmc.cb)
    return ("PRIVATE=%5.1fMB WS=%5.1fMB GDI=%d USER=%d" % (
        pmc.PrivateUsage / 1048576, pmc.WorkingSetSize / 1048576,
        u.GetGuiResources(h, 0), u.GetGuiResources(h, 1)))


subprocess.run([PY, os.path.join(ROOT, "restore_desktop.py")], capture_output=True)
subprocess.run(["taskkill", "/F", "/IM", "meow-desk.exe"], capture_output=True)
time.sleep(0.5)
p = subprocess.Popen([EXE], cwd=ROOT)
time.sleep(5)
h = ctypes.windll.kernel32.OpenProcess(0x1000 | 0x0400, False, p.pid)
print("t0   ", sample(h))

# 用鼠标在空白桌面反复划选框
def click_drag(x0, y0, x1, y1):
    u.SetCursorPos(x0, y0)
    time.sleep(0.05)
    u.mouse_event(0x0002, 0, 0, 0, 0)          # LEFTDOWN
    steps = 6
    for i in range(1, steps + 1):
        u.SetCursorPos(x0 + (x1 - x0) * i // steps, y0 + (y1 - y0) * i // steps)
        time.sleep(0.02)
    u.mouse_event(0x0004, 0, 0, 0, 0)          # LEFTUP
    time.sleep(0.05)

for r in range(6):
    click_drag(1900, 200, 2100, 500)
    click_drag(2400, 900, 2200, 700)
print("t1   ", sample(h), "(12 次框选后)")

for r in range(10):
    u.SetCursorPos(1000, 20); time.sleep(0.02)
print("t2   ", sample(h), "(长时间空闲前)")

time.sleep(25)
print("t3   ", sample(h), "(空闲 25s 后)")

subprocess.run(["taskkill", "/F", "/IM", "meow-desk.exe"], capture_output=True)
time.sleep(0.4)
subprocess.run([PY, os.path.join(ROOT, "restore_desktop.py")], capture_output=True)

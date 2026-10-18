"""内存分解：逐项关掉某类数据，看 private 掉多少"""
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


def run(tag, extra=None):
    env = dict(os.environ)
    env.pop("MEOW_SURFACE", None)
    if extra:
        env.update(extra)
    subprocess.run([PY, os.path.join(ROOT, "restore_desktop.py")], capture_output=True)
    subprocess.run(["taskkill", "/F", "/IM", "meow-desk.exe"], capture_output=True)
    time.sleep(0.5)
    p = subprocess.Popen([EXE], cwd=ROOT, env=env)
    time.sleep(4.5)
    if p.poll() is not None:
        print(tag, "EXITED", p.returncode)
        return
    h = ctypes.windll.kernel32.OpenProcess(0x1000 | 0x0400, False, p.pid)
    pmc = PMC(); pmc.cb = ctypes.sizeof(PMC)
    ctypes.windll.psapi.GetProcessMemoryInfo(h, ctypes.byref(pmc), pmc.cb)
    print("%-28s WS=%5.1f  PRIVATE=%5.1f" % (tag, pmc.WorkingSetSize / 1048576,
                                             pmc.PrivateUsage / 1048576))
    subprocess.run(["taskkill", "/F", "/IM", "meow-desk.exe"], capture_output=True)
    time.sleep(0.4)
    subprocess.run([PY, os.path.join(ROOT, "restore_desktop.py")], capture_output=True)


run("A 基线(当前默认)")
run("B 不建 DIB 缓存", {"MEOW_PROBE_NOCACHE": "1"})
run("C 不建缓存 + 不载图标", {"MEOW_PROBE_NOCACHE": "1", "MEOW_PROBE_NOICON": "1"})
run("D 分层(旧)模式", {"MEOW_SURFACE": "layered"})
run("E 分层 + 无缓存 + 无图标", {"MEOW_SURFACE": "layered", "MEOW_PROBE_NOCACHE": "1",
                                 "MEOW_PROBE_NOICON": "1"})

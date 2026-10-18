import ctypes, time, re, sys
from ctypes import wintypes

u = ctypes.windll.user32
LOG = r"C:\Users\22357\Desktop\meow-desk\meow-desk-core\build\meow-desk.log"

# 找窗口
surface = u.FindWindowW("MeowDeskSurface", None)
fence = u.FindWindowW(None, "meow-desk-fence")
print("surface", surface, "fence", fence)
if not surface or not fence:
    print("window not found"); sys.exit(1)

# 从日志拿第一个图标坐标
txt = open(LOG, encoding="utf-8", errors="ignore").read()
m = re.search(r"at \((-?\d+),(-?\d+)\)", txt)
if not m:
    print("no icon pos in log"); sys.exit(1)
ix, iy = int(m.group(1)), int(m.group(2))
print("first icon at screen", ix, iy)

# surface 窗口原点
r = wintypes.RECT(); u.GetWindowRect(surface, ctypes.byref(r))
ox, oy = r.left, r.top
# 图标格子中心（客户区坐标）
cx = ix - ox + 38
cy = iy - oy + 30

# 盒子中心
fr = wintypes.RECT(); u.GetWindowRect(fence, ctypes.byref(fr))
tx = (fr.left + fr.right) // 2 - ox
ty = (fr.top + fr.bottom) // 2 - oy
print(f"drag from ({cx},{cy}) to ({tx},{ty})")

def post(msg, x, y, wp=0):
    lp = ((y & 0xFFFF) << 16) | (x & 0xFFFF)
    u.PostMessageW(surface, msg, wp, lp)

# 先截图对比前
import subprocess
py = r"C:\Users\22357\.workbuddy\binaries\python\envs\default\Scripts\python.exe"
subprocess.run([py, "-c", "from PIL import ImageGrab; ImageGrab.grab().crop((%d,%d,%d,%d)).save(r'C:/Users/22357/Desktop/meow-desk/meow-desk-core/before_drop.png')" % (fr.left, fr.top, fr.right, fr.bottom)])

post(0x0201, cx, cy, 1)      # LBUTTONDOWN
time.sleep(0.15)
post(0x0200, cx + 20, cy + 10)   # MOUSEMOVE (超过阈值)
time.sleep(0.15)
post(0x0200, tx, ty)             # MOUSEMOVE 到盒子
time.sleep(0.2)
post(0x0202, tx, ty, 0)          # LBUTTONUP
time.sleep(1.0)

subprocess.run([py, "-c", "from PIL import ImageGrab; ImageGrab.grab().crop((%d,%d,%d,%d)).save(r'C:/Users/22357/Desktop/meow-desk/meow-desk-core/after_drop.png')" % (fr.left, fr.top, fr.right, fr.bottom)])
print("done")

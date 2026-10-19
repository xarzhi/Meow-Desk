#!/usr/bin/env python3
"""从 logo.png 生成程序图标：res/meow-desk.ico + res/logo-256.png + res/logo-128.png

原则（用户明确要求，别再自作聪明）：
**logo.png 整张原样当图标，只做等比缩放。不抠图、不重绘、不改构图、
不缩紧包围盒、不挑其中某一块出来用。**

logo.png 是 512x512 的四宫格设计稿（左上白底猫头，另外三张是紫色圆角砖）。
把整张缩成图标，出来的就是「四个格子」的样子 —— 这就是用户要的效果，
他觉得那才是他给的图。以前我"聪明"过一次：把左上那块猫单独取出来当图标，
被明确否掉了（「你他妈的为什么把托盘图标改成裁剪的了」）。

（更早还量过猫的墨迹包围盒、贴着裁掉空白、再在自绘底板上正片叠底重画 ——
那既改构图又丢描边，更不能用。别写回来。）

依赖：python3 + ffmpeg（用来解码/缩放，脚本里会在几个常见位置找一个）

用法：
    python tools/make_icon.py
产物：
    res/meow-desk.ico     exe / 托盘 / 窗口图标（9 个尺寸，≤64 用 BMP，128/256 用 PNG）
    res/logo-256.png      256 版（备用）
    res/logo-128.png      关于页显示的 app 图标（RC 编进 exe）
"""

import os
import shutil
import struct
import subprocess
import sys

CORE = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SRC = os.path.join(CORE, "logo.png")
RES = os.path.join(CORE, "res")
TMP = os.path.join(CORE, "_tmp_ico")

# 整张原图直接缩，不取其中任何一块。
#
# 这里曾经是 (77, 76, 156) —— 四宫格左上那块猫。用户明确否掉了：
# 他要的就是 logo.png 这张图本身，裁出来的一块不算。别再改回去。
TILE = None

# 这台机器上 ffmpeg 不在 PATH，几个程序自带的副本都能用
FFMPEG_CANDIDATES = [
    shutil.which("ffmpeg"),
    r"C:\Users\22357\AppData\Local\Programs\recordly\resources\app.asar.unpacked"
    r"\node_modules\ffmpeg-static\ffmpeg.exe",
    r"D:\Program Files\toolknit-desktop\resources\ffmpeg\ffmpeg.exe",
    r"D:\Program Files\EVCapture\ffmpeg.exe",
]

SIZES = [16, 20, 24, 32, 40, 48, 64, 128, 256]
# 缩小用 area（区域平均）：细线条缩到 16px 时比 lanczos 干净，不会 ringing 出白边
SCALE_FLAGS = "area"


def find_ffmpeg():
    for p in FFMPEG_CANDIDATES:
        if p and os.path.exists(p):
            return p
    sys.exit("ffmpeg not found; install it or edit FFMPEG_CANDIDATES")


FFMPEG = find_ffmpeg()


def ff(*args):
    subprocess.run([FFMPEG, "-v", "error", "-y"] + list(args), check=True)


def raw_rgba(path):
    """解码成 raw RGBA，同时读出宽高。"""
    with open(path, "rb") as f:
        head = f.read(33)
    w, h = struct.unpack(">II", head[16:24])
    out = subprocess.run(
        [FFMPEG, "-v", "error", "-i", path, "-f", "rawvideo", "-pix_fmt", "rgba", "-"],
        capture_output=True, check=True).stdout
    assert len(out) == w * h * 4, (path, len(out), w, h)
    return bytearray(out), w, h


def png_size(path):
    """只读 PNG 头里的宽高，不解码。"""
    with open(path, "rb") as f:
        head = f.read(24)
    return struct.unpack(">II", head[16:24])


def scale(src, dst, size):
    """整张图等比缩放到 size×size，保留 alpha。"""
    ff("-i", src, "-vf", f"scale={size}:{size}:flags={SCALE_FLAGS}", dst)


def ico_bmp_entry(rgba, w, h):
    """ICO 里的 32bpp BMP：头里的高度写两倍，像素自下而上，再跟一张 AND 掩码。"""
    hdr = struct.pack("<IiiHHIIiiII", 40, w, h * 2, 1, 32, 0, w * h * 4, 0, 0, 0, 0)
    stride = ((w + 31) // 32) * 4
    xor = bytearray(w * h * 4)
    mask = bytearray(stride * h)
    for y in range(h):
        srow = y * w * 4
        drow = (h - 1 - y) * w * 4
        mrow = (h - 1 - y) * stride
        for x in range(w):
            si = srow + x * 4
            di = drow + x * 4
            xor[di] = rgba[si + 2]        # B
            xor[di + 1] = rgba[si + 1]    # G
            xor[di + 2] = rgba[si]        # R
            xor[di + 3] = rgba[si + 3]    # A
            if rgba[si + 3] < 128:
                mask[mrow + (x >> 3)] |= 0x80 >> (x & 7)
    return hdr + bytes(xor) + bytes(mask)


def main():
    if not os.path.exists(SRC):
        sys.exit("logo.png not found at " + SRC)
    os.makedirs(RES, exist_ok=True)
    os.makedirs(TMP, exist_ok=True)

    # 整张原图进，整张缩。没有任何裁切/取块步骤。
    src = SRC

    # 每个尺寸 = 等比缩放，不做任何重新构图
    scaled = {}
    for s in SIZES:
        p = os.path.join(TMP, f"s{s}.png")
        scale(src, p, s)
        scaled[s] = p
    print("scaled", " ".join(str(s) for s in SIZES), flush=True)

    # 关于页 / 备用图：同样直接从原图缩
    shutil.copyfile(scaled[256], os.path.join(RES, "logo-256.png"))
    shutil.copyfile(scaled[128], os.path.join(RES, "logo-128.png"))

    entries = []
    for s in SIZES:
        if s <= 64:
            # ≤64 用 BMP：老 API 不认 PNG 压缩的 ICO 项
            rgba, w, h = raw_rgba(scaled[s])
            data = ico_bmp_entry(rgba, w, h)
        else:
            data = open(scaled[s], "rb").read()   # 128 / 256 用 PNG 省体积
        entries.append((s, data))

    out = bytearray(struct.pack("<HHH", 0, 1, len(entries)))
    offset = 6 + 16 * len(entries)
    for s, data in entries:
        bw = 0 if s >= 256 else s       # 256 在目录项里写 0
        out += struct.pack("<BBBBHHII", bw, bw, 0, 0, 1, 32, len(data), offset)
        offset += len(data)
    for _, data in entries:
        out += data
    ico = os.path.join(RES, "meow-desk.ico")
    open(ico, "wb").write(out)
    print("ico bytes", len(out), "->", ico)

    # 中间产物不留在仓库里（_tmp_ico 是纯临时目录，每次跑都会重建）
    shutil.rmtree(TMP, ignore_errors=True)


if __name__ == "__main__":
    main()

#!/usr/bin/env python3
"""从 logo.png 生成程序图标：res/meow-desk.ico + res/logo-256.png + res/logo-128.png

原则：**原图怎么画就怎么用，只做等比缩放。不裁、不重新合成、不改构图。**

logo.png 本身是 512x512 RGBA 的标准图标稿：圆角底板铺满整块画布、四角透明，
所以每个尺寸都是「整张图缩到 N×N」就完事。

（早期版本量出猫的墨迹包围盒、贴着裁掉空白、再在自绘底板上正片叠底重画 ——
那会改变猫在底板里的占比，还会丢掉原图的描边和内阴影，等于擅自改了设计稿。
已废弃，别再写回来。）

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

    # 每个尺寸 = 整张原图的等比缩放，不做任何裁切 / 重排
    scaled = {}
    for s in SIZES:
        p = os.path.join(TMP, f"s{s}.png")
        scale(SRC, p, s)
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


if __name__ == "__main__":
    main()

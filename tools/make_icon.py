#!/usr/bin/env python3
"""从 logo.png 生成程序图标：res/meow-desk.ico + res/logo-256.png + res/logo-128.png

原则（用户明确要求，别再自作聪明）：
**logo.png 整张原样当图标，只做等比缩放。不抠图、不重绘、不改构图、
不缩紧包围盒、不挑其中某一块出来用。**

logo.png 是 512x512 的四宫格设计稿（左上白底猫头，另外三张是紫色圆角砖）。
把整张缩成图标，出来的就是「四个格子」的样子 —— 这就是用户要的效果，
他觉得那才是他给的图。以前我"聪明"过一次：把左上那块猫单独取出来当图标，
被明确否掉了（「你他妈的为什么把托盘图标的图标都给我改成裁剪的了」）。

非正方形的图：等比缩到 size 以内，居中放进 size×size 的**透明**画布。
补透明边不算改图，裁切才算。

依赖：python3 + ffmpeg（用来解码/缩放，脚本里会在几个常见位置找一个）

用法：
    python tools/make_icon.py                    # 用当前 logo.png 重新生成
    python tools/make_icon.py --install 图.png    # 先把这张图换成 logo.png，再生成
    python tools/make_icon.py --no-build-hint     # 只生成，不打印「记得重新编译」
产物：
    res/meow-desk.ico     exe / 托盘 / 窗口图标（9 个尺寸，≤64 用 BMP，128/256 用 PNG）
    res/logo-256.png      256 版（备用）
    res/logo-128.png      关于页显示的 app 图标（RC 编进 exe）

换完图标必须重新编译 exe（资源是编进去的），要么跑 ./set-icon.sh，要么 ./build.sh。
"""

import os
import shutil
import struct
import subprocess
import sys
import time

CORE = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SRC = os.path.join(CORE, "logo.png")
RES = os.path.join(CORE, "res")
TMP = os.path.join(CORE, "_tmp_ico")
BUILD = os.path.join(CORE, "build")

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
    sys.exit("找不到 ffmpeg；装一个，或者改脚本里的 FFMPEG_CANDIDATES")


FFMPEG = None   # 懒加载：只跑 --help 时不用去找 ffmpeg


def ensure_ffmpeg():
    global FFMPEG
    if FFMPEG is None:
        FFMPEG = find_ffmpeg()
    return FFMPEG


def ff(*args):
    subprocess.run([ensure_ffmpeg(), "-v", "error", "-y"] + list(args), check=True)


def png_size(path):
    """只读 PNG 头里的宽高，不解码。"""
    with open(path, "rb") as f:
        head = f.read(24)
    if len(head) < 24 or head[:8] != b"\x89PNG\r\n\x1a\n":
        sys.exit(f"不是 PNG：{path}")
    return struct.unpack(">II", head[16:24])


def raw_rgba(path):
    """解码成 raw RGBA，同时读出宽高。"""
    w, h = png_size(path)
    out = subprocess.run(
        [ensure_ffmpeg(), "-v", "error", "-i", path,
         "-f", "rawvideo", "-pix_fmt", "rgba", "-"],
        capture_output=True, check=True).stdout
    assert len(out) == w * h * 4, (path, len(out), w, h)
    return bytearray(out), w, h


def to_png(src, dst):
    """任意 ffmpeg 能解的图 → PNG（保留 alpha）。已经是 PNG 就原样复制。"""
    if os.path.abspath(src) == os.path.abspath(dst):
        return
    if src.lower().endswith(".png"):
        shutil.copyfile(src, dst)
        return
    ff("-i", src, dst)


def install_logo(path):
    """把给定的图换成 logo.png（旧的备份进 build/，不留在仓库里）。"""
    if not os.path.exists(path):
        sys.exit(f"图片不存在：{path}")
    if os.path.abspath(path) == os.path.abspath(SRC):
        print(f"{os.path.basename(path)} 就是 logo.png 本身，跳过替换")
        return
    os.makedirs(BUILD, exist_ok=True)
    if os.path.exists(SRC):
        bak = os.path.join(BUILD, "logo.prev." + time.strftime("%m%d-%H%M%S") + ".png")
        shutil.copyfile(SRC, bak)
        print("旧 logo.png 备份到 " + os.path.relpath(bak, CORE))
    to_png(path, SRC)
    print("已用 " + path + " 替换 logo.png")


def scale(src, dst, size, square):
    """等比缩放到 size×size。非正方形图居中补透明边（不裁切、不拉伸）。"""
    if square:
        vf = "scale=%d:%d:flags=%s" % (size, size, SCALE_FLAGS)
    else:
        vf = ("scale=%d:%d:force_original_aspect_ratio=decrease:flags=%s,"
              "pad=%d:%d:(ow-iw)/2:(oh-ih)/2:color=black@0"
              % (size, size, SCALE_FLAGS, size, size))
    ff("-i", src, "-vf", vf, dst)


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


def main(argv):
    install = None
    hints = True
    i = 0
    while i < len(argv):
        a = argv[i]
        if a in ("-h", "--help"):
            print(__doc__)
            return 0
        if a == "--install":
            i += 1
            if i >= len(argv):
                sys.exit("--install 后面要跟图片路径")
            install = argv[i]
        elif a == "--no-build-hint":
            hints = False
        else:
            sys.exit("不认识的参数：" + a + "（--help 看用法）")
        i += 1

    if install:
        install_logo(install)

    if not os.path.exists(SRC):
        sys.exit("logo.png 不存在：" + SRC)

    os.makedirs(RES, exist_ok=True)
    os.makedirs(TMP, exist_ok=True)

    # 整张原图进，整张缩。没有任何裁切/取块步骤。
    src = SRC
    w, h = png_size(SRC)
    square = (w == h)
    if not square:
        print("注意：图是 %dx%d（不是正方形），各尺寸会等比缩小后居中补透明边，"
              "不裁切、不拉伸" % (w, h), flush=True)

    scaled = {}
    for s in SIZES:
        p = os.path.join(TMP, "s%d.png" % s)
        scale(src, p, s, square)
        scaled[s] = p
    print("scaled", " ".join(str(s) for s in SIZES), flush=True)

    # 关于页 / 备用图：同样直接从原图缩
    shutil.copyfile(scaled[256], os.path.join(RES, "logo-256.png"))
    shutil.copyfile(scaled[128], os.path.join(RES, "logo-128.png"))

    entries = []
    for s in SIZES:
        if s <= 64:
            # ≤64 用 BMP：老 API 不认 PNG 压缩的 ICO 项
            rgba, ww, hh = raw_rgba(scaled[s])
            data = ico_bmp_entry(rgba, ww, hh)
        else:
            with open(scaled[s], "rb") as f:
                data = f.read()   # 128 / 256 用 PNG 省体积
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
    with open(ico, "wb") as f:
        f.write(out)
    print("ico %d bytes -> %s" % (len(out), os.path.relpath(ico, CORE)))

    shutil.rmtree(TMP, ignore_errors=True)

    if hints:
        print("")
        print("资源已更新。exe 里的图标是编译时编进去的，要重新编译才生效：")
        print("  ./set-icon.sh        （换图 + 生成 + 编译 + 自检，一步到位）")
        print("  或 ./build.sh         （只编译）")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))

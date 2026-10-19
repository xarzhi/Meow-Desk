#!/usr/bin/env python3
"""从 logo.png 生成程序图标：res/meow-desk.ico + res/logo-256.png + res/logo-128.png

为什么需要这个脚本：
    设计稿 logo.png 是「白底圆角底板 + 一只猫」的 512px 方图。直接缩成 16px 之后，
    白底板和桌面/任务栏背景糊成一片，看不出形状；外圈那层描边缩下来还会变成脏边。
    所以这里重做一遍：量出猫的墨迹包围盒 → 贴着它裁 → 在**我们自己的**圆角底板上
    用「正片叠底」把猫压上去（白底自然消失）→ 导出 9 个尺寸的 ICO。

依赖：python3 + ffmpeg（用来解码/缩放，脚本里会在几个常见位置找一个）

用法：
    python tools/make_icon.py
产物：
    res/meow-desk.ico     exe / 托盘 / 窗口图标（9 个尺寸，≤64 用 BMP，128/256 用 PNG）
    res/logo-256.png      圆角底板版（备用）
    res/logo-128.png      关于页显示的 app 图标（RC 编进 exe）
"""

import math
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

W = H = 512     # logo.png 的尺寸
S = 256         # 生成的主图边长
SIZES = [16, 20, 24, 32, 40, 48, 64, 128, 256]


def find_ffmpeg():
    for p in FFMPEG_CANDIDATES:
        if p and os.path.exists(p):
            return p
    sys.exit("ffmpeg not found; install it or edit FFMPEG_CANDIDATES")


FFMPEG = find_ffmpeg()


def ff(*args):
    subprocess.run([FFMPEG, "-v", "error", "-y"] + list(args), check=True)


def raw(path, w, h):
    out = subprocess.run(
        [FFMPEG, "-v", "error", "-i", path, "-f", "rawvideo", "-pix_fmt", "rgba", "-"],
        capture_output=True, check=True).stdout
    assert len(out) == w * h * 4, (path, len(out))
    return bytearray(out)


def row_span(fy, radius):
    """圆角方块在第 fy 行（行中心）的 x 范围 [xl, xr)。"""
    dy = 0.0
    if fy < radius:
        dy = radius - fy
    elif fy > S - radius:
        dy = fy - (S - radius)
    if dy <= 0:
        return 0, S
    dx = math.sqrt(max(0.0, radius * radius - dy * dy))
    return int(round(radius - dx)), int(round(S - radius + dx))


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
            xor[di] = rgba[si + 2]
            xor[di + 1] = rgba[si + 1]
            xor[di + 2] = rgba[si]
            xor[di + 3] = rgba[si + 3]
            if rgba[si + 3] < 128:
                mask[mrow + (x >> 3)] |= 0x80 >> (x & 7)
    return hdr + bytes(xor) + bytes(mask)


def main():
    if not os.path.exists(SRC):
        sys.exit("logo.png not found at " + SRC)
    os.makedirs(RES, exist_ok=True)
    os.makedirs(TMP, exist_ok=True)

    src = raw(SRC, W, H)
    print("read logo", flush=True)

    # ---- 1. 猫的墨迹包围盒（蓝色描边，白色底/粉色耳朵都不命中）----
    minx, miny, maxx, maxy = 10 ** 9, 10 ** 9, -1, -1
    for y in range(256):
        row = y * W * 4
        for x in range(256):
            i = row + x * 4
            if src[i + 2] - src[i] > 60:            # b - r
                minx = min(minx, x)
                maxx = max(maxx, x)
                miny = min(miny, y)
                maxy = max(maxy, y)
    print("cat ink", minx, miny, maxx, maxy, maxx - minx + 1, maxy - miny + 1, flush=True)

    # 贴着墨迹包围盒裁（不留余量）：原图那块白底圆角底板的描边/内阴影就在墨迹外侧
    # 几个像素处，一旦留余量就会把那一圈边框也裁进来，贴到我们自己的底板上会出现
    # 一个「里面还有一个方块」的鬼影。
    m = 0
    cx0, cy0 = max(0, minx - m), max(0, miny - m)
    cw = min(256 - cx0, maxx + m - cx0 + 1)
    ch = min(256 - cy0, maxy + m - cy0 + 1)

    cat_px = round(S * 0.76)   # 猫在底板里占多宽
    sc = cat_px / max(cw, ch)
    dw, dh = max(1, round(cw * sc)), max(1, round(ch * sc))
    ff("-i", SRC, "-vf", f"crop={cw}:{ch}:{cx0}:{cy0},scale={dw}:{dh}:flags=lanczos",
       "-f", "rawvideo", "-pix_fmt", "rgba", os.path.join(TMP, "cat.rgba"))
    cat = bytearray(open(os.path.join(TMP, "cat.rgba"), "rb").read())
    print("cat scaled", dw, dh, flush=True)

    # ---- 2. 底板：圆角方块 + 极淡竖向渐变 + 2px 描边 ----
    radius = S * 0.225
    border = bytes((232, 234, 242, 255))
    canvas = bytearray(S * S * 4)
    for y in range(S):
        fy = y + 0.5
        t = y / (S - 1)
        inner = bytes((round(252 - 6 * t), round(253 - 5 * t), 255, 255))
        xl, xr = row_span(fy, radius)
        if xr <= xl:
            continue
        ix0, ix1 = xl + 2, xr - 2          # 左右各留 2px 描边
        base = y * S * 4
        canvas[base + xl * 4: base + xr * 4] = border * (xr - xl)
        if ix1 > ix0:
            canvas[base + ix0 * 4: base + ix1 * 4] = inner * (ix1 - ix0)
    print("tile ok", flush=True)

    # ---- 3. 猫按「正片叠底」贴上去：白底自然消失，边缘不会留白框 ----
    ox = (S - dw) // 2
    oy = (S - dh) // 2 - round(S * 0.005)
    for yy in range(dh):
        ty = oy + yy
        if not (0 <= ty < S):
            continue
        sbase = yy * dw * 4
        dbase = (ty * S + ox) * 4
        for xx in range(dw):
            si = sbase + xx * 4
            sa = cat[si + 3]
            if sa < 4:
                continue
            f = sa / 255.0
            di = dbase + xx * 4
            for k in range(3):
                v = canvas[di + k]
                s = cat[si + k] / 255.0
                canvas[di + k] = round(v * (s * f + (1 - f)))
    print("composited", flush=True)

    master = os.path.join(RES, "logo-256.png")
    subprocess.run([FFMPEG, "-v", "error", "-y", "-f", "rawvideo", "-pix_fmt", "rgba",
                    "-s", f"{S}x{S}", "-i", "-", master], input=bytes(canvas), check=True)
    ff("-i", master, "-vf", "scale=128:128:flags=lanczos", os.path.join(RES, "logo-128.png"))

    # ---- 4. 多尺寸 ICO ----
    entries = []
    for s in SIZES:
        p = os.path.join(TMP, f"s{s}.png")
        ff("-i", master, "-vf", f"scale={s}:{s}:flags=lanczos", p)
        # ≤64 用 BMP（老 API 不认 PNG 压缩的 ICO 项），128/256 用 PNG 省体积
        data = ico_bmp_entry(raw(p, s, s), s, s) if s <= 64 else open(p, "rb").read()
        entries.append((s, data))
    print("entries ok", flush=True)

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

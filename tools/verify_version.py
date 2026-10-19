#!/usr/bin/env python3
"""检查 exe 里到底有没有版本资源（右键 → 属性 → 详细信息 读的就是它）。

为什么要单独验：VERSIONINFO 是**资源**，不是编译进代码的常量。
rc 生成失败、rc 没被加进 target、或者 version.txt 跟 rc 对不上时，
编译器一个字都不会抱怨，只有打开属性页才发现「此文件没有可用的版本信息」。

用法：
    python tools/verify_version.py [exe ...]
不给参数就查 build/meow-desk.exe 和 build/meow-setup.exe。

返回：0 全部正常，1 有问题（缺资源 / 版本号跟 version.txt 不一致）
"""

import ctypes
import os
import subprocess
import sys
from ctypes import wintypes

CORE = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BUILD = os.path.join(CORE, "build")

# 属性页里能看到的字段；这几项一旦空着，用户看到的就是一片空白
WANT = ("FileDescription", "FileVersion", "ProductName", "ProductVersion",
        "OriginalFilename", "CompanyName")


def _read_file_version(path):
    """返回 (固定版本四元组, {字段: 值})；没有版本资源时返回 (None, {})。"""
    ver = ctypes.WinDLL("version")
    size = ver.GetFileVersionInfoSizeW(path, None)
    if not size:
        return None, {}

    buf = ctypes.create_string_buffer(size)
    if not ver.GetFileVersionInfoW(path, 0, size, buf):
        return None, {}

    # --- 固定信息：dwFileVersionMS/LS ---
    ptr = ctypes.c_void_p()
    length = wintypes.UINT()
    fixed = None
    if ver.VerQueryValueW(buf, "\\", ctypes.byref(ptr), ctypes.byref(length)):
        # VS_FIXEDFILEINFO: 6 个 DWORD 之后才是 dwFileVersionMS/LS
        vals = ctypes.cast(ptr, ctypes.POINTER(wintypes.DWORD))
        ms, ls = vals[2], vals[3]
        fixed = (ms >> 16, ms & 0xFFFF, ls >> 16, ls & 0xFFFF)

    # --- 字符串：不要写死 080404b0，按 Translation 拿到实际语言页 ---
    langs = []
    if ver.VerQueryValueW(buf, "\\VarFileInfo\\Translation",
                          ctypes.byref(ptr), ctypes.byref(length)):
        n = length.value // 4
        codes = ctypes.cast(ptr, ctypes.POINTER(wintypes.WORD))
        langs = [(codes[i * 2], codes[i * 2 + 1]) for i in range(n)]

    out = {}
    for lang, cp in langs:
        for key in WANT:
            sub = "\\StringFileInfo\\%04x%04x\\%s" % (lang, cp, key)
            if ver.VerQueryValueW(buf, sub, ctypes.byref(ptr), ctypes.byref(length)):
                out[key] = ctypes.wstring_at(ptr, length.value).rstrip("\x00")
    return fixed, out


def _version_txt():
    p = os.path.join(CORE, "version.txt")
    try:
        with open(p, encoding="utf-8") as f:
            return f.read().strip()
    except OSError:
        return ""


def main(argv):
    targets = argv or [os.path.join(BUILD, "meow-desk.exe"),
                       os.path.join(BUILD, "meow-setup.exe")]
    want = _version_txt()
    bad = 0

    for path in targets:
        name = os.path.basename(path)
        if not os.path.exists(path):
            print("[FAIL] %s 不存在（先编译）" % name)
            bad += 1
            continue

        fixed, info = _read_file_version(path)
        if fixed is None:
            print("[FAIL] %s 里没有版本资源 —— 右键属性会显示"
                  "「此文件没有可用的版本信息」" % name)
            bad += 1
            continue

        missing = [k for k in WANT if not info.get(k)]
        quad = "%d.%d.%d.%d" % fixed
        ok = not missing and (not want or info.get("FileVersion") == want)
        print("[%s] %s  FileVersion=%s (固定版本 %s)  %s"
              % ("OK  " if ok else "FAIL", name, info.get("FileVersion"), quad,
                 info.get("FileDescription")))
        if missing:
            print("       缺字段：%s" % ", ".join(missing))
        if want and info.get("FileVersion") != want:
            print("       version.txt 是 %s，exe 里却是 %s —— 重新 configure 再编译"
                  % (want, info.get("FileVersion")))
        if not ok:
            bad += 1

    # Git Bash 里 python 的参数被当路径改写时，中文输出会乱码，统一转一下由调用方处理
    return 0 if bad == 0 else 1


if __name__ == "__main__":
    try:
        sys.stdout.reconfigure(errors="replace")
    except Exception:
        pass
    sys.exit(main(sys.argv[1:]))

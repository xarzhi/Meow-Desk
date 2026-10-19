#!/usr/bin/env python3
"""校验「exe 里编进去的图标」是不是就是 res/meow-desk.ico 这一份。

为什么需要它：rc.exe 把 ico 编进 exe 之后，肉眼没法确认里面到底是哪一版。
换了图标却没生效（ninja 认为资源没变 / 编到了另一个 exe / 手上跑的是旧副本）
这种情况不会有任何报错，只能靠逐字节比对。

做法：把 exe 当数据文件 LoadLibrary，枚举 RT_ICON(3)，把每一项的字节和 ico
里对应尺寸的字节比对。9 个尺寸全部命中才算通过。

用法：
    python tools/verify_icon.py                  # 查 build/ 下两个 exe
    python tools/verify_icon.py 路径.exe ...      # 查指定 exe
退出码：0 全部一致；1 有不一致 / 缺项。
"""

import ctypes
import os
import struct
import sys
from ctypes import wintypes

CORE = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ICO = os.path.join(CORE, "res", "meow-desk.ico")
DEFAULT_EXES = [os.path.join(CORE, "build", "meow-desk.exe"),
                os.path.join(CORE, "build", "meow-setup.exe")]

RT_ICON = 3
LOAD_LIBRARY_AS_DATAFILE = 0x2

k32 = ctypes.WinDLL("kernel32", use_last_error=True)
k32.LoadLibraryExW.restype = wintypes.HMODULE
k32.LoadLibraryExW.argtypes = [wintypes.LPCWSTR, wintypes.HANDLE, wintypes.DWORD]
k32.FindResourceW.restype = wintypes.HANDLE
k32.FindResourceW.argtypes = [wintypes.HMODULE, ctypes.c_void_p, ctypes.c_void_p]
k32.LoadResource.restype = wintypes.HANDLE
k32.LoadResource.argtypes = [wintypes.HMODULE, wintypes.HANDLE]
k32.LockResource.restype = ctypes.c_void_p
k32.LockResource.argtypes = [wintypes.HANDLE]
k32.SizeofResource.restype = wintypes.DWORD
k32.SizeofResource.argtypes = [wintypes.HMODULE, wintypes.HANDLE]

ENUMPROC = ctypes.WINFUNCTYPE(wintypes.BOOL, wintypes.HMODULE,
                              ctypes.c_void_p, ctypes.c_void_p, ctypes.c_void_p)
k32.EnumResourceNamesW.argtypes = [wintypes.HMODULE, ctypes.c_void_p,
                                   ENUMPROC, ctypes.c_void_p]


def exe_icons(path):
    """返回 {资源名: bytes}。整数资源 ID 必须用 c_void_p 传，直接 cast 成
    LPCWSTR 会按高位判断成字符串指针，报 OverflowError。"""
    h = k32.LoadLibraryExW(path, None, LOAD_LIBRARY_AS_DATAFILE)
    if not h:
        sys.exit("LoadLibraryEx 失败：%s (err=%d)" % (path, ctypes.get_last_error()))
    out = {}

    def cb(mod, typ, name, param):
        r = k32.FindResourceW(mod, name, ctypes.c_void_p(RT_ICON))
        if not r:
            return True
        sz = k32.SizeofResource(mod, r)
        data = ctypes.string_at(k32.LockResource(k32.LoadResource(mod, r)), sz)
        out[name if name is not None else 0] = bytes(data)
        return True

    k32.EnumResourceNamesW(h, ctypes.c_void_p(RT_ICON), ENUMPROC(cb), None)
    return out


def ico_entries(path):
    with open(path, "rb") as f:
        raw = f.read()
    _, _, n = struct.unpack("<HHH", raw[:6])
    res = []
    for i in range(n):
        off = 6 + 16 * i
        w, h, _, _, _, bpp, size, addr = struct.unpack("<BBBBHHII", raw[off:off + 16])
        res.append((w or 256, raw[addr:addr + size]))
    return res


def check(exe, want):
    if not os.path.exists(exe):
        print("  [跳过] %s 不存在" % exe)
        return True
    got = exe_icons(exe)
    pool = set(got.values())
    bad = []
    print("%s（RT_ICON %d 项）" % (os.path.relpath(exe, CORE), len(got)))
    for size, data in want:
        ok = data in pool
        if not ok:
            bad.append(size)
        print("  %3dx%-3d %7d bytes  %s" % (size, size, len(data), "OK" if ok else "不一致"))
    if bad:
        print("  → %d 项对不上：%s（exe 里的图标不是当前 res/meow-desk.ico，"
              "要重新编译）" % (len(bad), bad))
    else:
        print("  → 与 res/meow-desk.ico 完全一致")
    return not bad


def main(argv):
    if argv and argv[0] in ("-h", "--help"):
        print(__doc__)
        return 0
    if not os.path.exists(ICO):
        sys.exit("找不到 " + ICO + "，先跑 python tools/make_icon.py")
    want = ico_entries(ICO)
    print("参照：res/meow-desk.ico（%d 个尺寸，%d 字节）"
          % (len(want), os.path.getsize(ICO)))
    exes = argv if argv else [p for p in DEFAULT_EXES if os.path.exists(p)]
    if not exes:
        sys.exit("没有可查的 exe，先跑 ./build.sh")
    all_ok = True
    for e in exes:
        all_ok = check(e, want) and all_ok
    print("")
    print("结果：" + ("全部一致" if all_ok else "有不一致，见上"))
    return 0 if all_ok else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))

#!/usr/bin/env python3
"""编译 meow-desk（和 build.sh 等价，但不依赖 bash）。

为什么要有这个：build.sh 只能在 Git Bash 里跑，用户在 PowerShell / cmd 里
`./build.sh` 会报「无法识别」。这里用 python 直接驱动 cmake + ninja，
cmd、PowerShell、Git Bash 里都能跑，set-icon / make-setup 也复用它。

工具链路径固化在 msvc-toolchain.cmake（INCLUDE / LIB 都在里面按 flags 传），
所以不需要 vcvars64，也不需要 reg.exe。

用法：
    python tools/build.py

返回：0 成功，1 失败（失败会把最后 40 行日志打出来并给人话原因）
"""

import os
import shutil
import subprocess
import sys

CORE = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BUILD = os.path.join(CORE, "build")
TOOLCHAIN = os.path.join(CORE, "msvc-toolchain.cmake")

# ninja 系统 PATH 里没有，VS 自带一份
NINJA_CANDIDATES = [
    r"C:\Program Files\Microsoft Visual Studio\18\Community\Common7\IDE"
    r"\CommonExtensions\Microsoft\CMake\Ninja\ninja.exe",
    r"C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\IDE"
    r"\CommonExtensions\Microsoft\CMake\Ninja\ninja.exe",
]

CMAKE_CANDIDATES = [
    r"D:\environment\CMake\bin\cmake.exe",
    r"C:\Program Files\CMake\bin\cmake.exe",
]


def find_tool(name, candidates):
    for p in candidates:
        if p and os.path.exists(p):
            return p
    return shutil.which(name)


def build(quiet=False):
    """跑一次增量编译。返回 (ok, 输出文本)。"""
    cmake = find_tool("cmake", CMAKE_CANDIDATES)
    if not cmake:
        return False, "找不到 cmake（试过 PATH、D:\\environment\\CMake、Program Files）"
    ninja = find_tool("ninja", NINJA_CANDIDATES)
    if not ninja:
        return False, "找不到 ninja（VS 自带的那个在 ...\\Common7\\IDE\\CommonExtensions\\Microsoft\\CMake\\Ninja\\）"

    os.makedirs(BUILD, exist_ok=True)
    env = dict(os.environ)

    steps = [
        [cmake, "-G", "Ninja",
         "-S", CORE, "-B", BUILD,
         "-DCMAKE_TOOLCHAIN_FILE=" + TOOLCHAIN,
         "-DCMAKE_BUILD_TYPE=Release",
         "-DCMAKE_MAKE_PROGRAM=" + ninja],
        [cmake, "--build", BUILD, "--config", "Release"],
    ]
    out = []
    for cmd in steps:
        p = subprocess.run(cmd, cwd=CORE, env=env, stdout=subprocess.PIPE,
                           stderr=subprocess.STDOUT, text=True, errors="replace")
        out.append(p.stdout or "")
        if p.returncode != 0:
            return False, "\n".join(out)
    return True, "\n".join(out)


def diagnose(log):
    """把编译器/链接器的报错翻译成人话。"""
    if "LNK1104" in log:
        # 从 "LNK1104: 无法打开文件“meow-setup.exe”" 里抠出到底谁被占用，
        # 以前一律写 meow-desk.exe，实际经常是 meow-setup.exe（安装器开着）
        import re
        m = re.search(r"LNK1104[^\u201c\u201d\n]*[\u201c\"]([^\u201d\"]+)[\u201d\"]", log)
        name = m.group(1) if m else "meow-desk.exe"
        who = "安装程序（meow-setup）" if "setup" in name else "主程序（meow-desk）"
        return ("%s 被占用：%s 还开着。\n"
                "  退出它再跑一次（脚本不替你杀进程）。\n"
                "  实在不方便退：把它改名占开也算（build\\%s → build\\%s.<时间>.exe）。"
                % (name, who, name, name.rsplit(".", 1)[0]))
    if "C1083" in log or "C1034" in log:
        return ("头文件 / 环境问题：确认 msvc-toolchain.cmake 里的 VS 和 Windows SDK 路径\n"
                "  还对不对（VS 升级后 MSVC 版本号会变）。")
    if "ninja: error" in log:
        return "ninja 报错，往上翻日志。"
    return ""


def main(argv):
    quiet = "-q" in argv or "--quiet" in argv
    ok, log = build(quiet)
    # 完整日志留一份，失败后可以直接翻
    try:
        with open(os.path.join(BUILD, "build.log"), "w", encoding="utf-8") as f:
            f.write(log)
    except OSError:
        pass

    if ok:
        print("Build done: build/meow-desk.exe")
        return 0

    print("---- 编译失败（完整日志：build/build.log）----")
    tail = log.strip().splitlines()[-40:]
    print("\n".join(tail))
    hint = diagnose(log)
    if hint:
        print("")
        print("→ " + hint)
    return 1


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))

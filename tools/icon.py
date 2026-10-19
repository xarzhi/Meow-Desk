#!/usr/bin/env python3
"""换程序图标：一张图进去，exe / 任务栏 / 托盘 / 关于页的图标全换掉。

用法（三种终端都能跑，见同目录的 set-icon.sh / set-icon.bat）：
    set-icon.bat 图片.png        <- cmd / PowerShell（双击也行）
    ./set-icon.sh 图片.png       <- Git Bash
    python tools/icon.py 图片.png

不带图片 = 用当前 logo.png 重新生成一遍。
非 PNG 也行（jpg / webp…），会先转成 PNG 再换。

选项：
    --no-build    只生成 res/ 里的图标资源，不编译
    --no-verify   跳过「exe 里的图标和 ico 是否一致」的校验

规则（用户明确要求过）：图是**整张**缩放的，不裁、不挑其中一块、不重绘。
旧 logo.png 会备份到 build/logo.prev.<时间>.png。
"""

import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
CORE = os.path.dirname(HERE)


def run(args, **kw):
    """跑一个子脚本，输出直接透传给用户。"""
    cmd = [sys.executable] + args
    return subprocess.run(cmd, cwd=CORE, **kw).returncode


def main(argv):
    img = None
    do_build = True
    do_verify = True
    for a in argv:
        if a in ("-h", "--help"):
            print(__doc__)
            return 0
        if a == "--no-build":
            do_build = False
        elif a == "--no-verify":
            do_verify = False
        elif a.startswith("-"):
            print("不认识的选项：%s（--help 看用法）" % a)
            return 2
        elif img is None:
            img = a
        else:
            print("只接受一张图片")
            return 2

    print("== 1/3 生成图标资源 ==")
    args = [os.path.join(HERE, "make_icon.py"), "--no-build-hint"]
    if img:
        if not os.path.exists(img):
            print("图片不存在：" + img)
            return 1
        args += ["--install", img]
    if run(args) != 0:
        return 1

    if not do_build:
        print("")
        print("--no-build：资源已更新，但没有编译。"
              "跑 build.bat / ./build.sh 才会进 exe。")
        return 0

    print("")
    print("== 2/3 编译 ==")
    if run([os.path.join(HERE, "build.py")]) != 0:
        return 1

    if do_verify:
        print("")
        print("== 3/3 自检：exe 里的图标是不是刚生成的那一份 ==")
        if run([os.path.join(HERE, "verify_icon.py")]) != 0:
            print("")
            print("自检没过：exe 里的图标和 res/meow-desk.ico 对不上。")
            return 1

    print("")
    print("完成。图标已换：exe / 任务栏 / 托盘 / 关于页 —— 全部来自 res/meow-desk.ico")
    print("")
    print("看效果前先退出正在运行的 meow-desk，再启动 build/meow-desk.exe。")
    print("要打包分发就跑 make-setup.bat（或 ./make-setup.sh）。")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))

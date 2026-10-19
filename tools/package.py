#!/usr/bin/env python3
"""打包成可分发的安装包（一条命令出 zip）。

用法（三种终端都能跑，见同目录的 make-setup.sh / make-setup.bat）：
    make-setup.bat              <- cmd / PowerShell（双击也行）
    ./make-setup.sh             <- Git Bash
    python tools/package.py

选项：
    --no-build    不编译，直接拿 build/ 里现成的 exe 打包
    --no-zip      只出目录，不压 zip
    --no-verify   跳过图标自检

产物：
    dist/meow-desk.exe + dist/meow-setup.exe   平铺一份（老习惯，直接能跑安装程序）
    dist/package/                               分发目录（两个 exe + 安装说明.txt）
    dist/MeowDesk-<版本>.zip                    发给别人就发这个

为什么两个 exe 必须放一起：meow-setup.exe 是从「它自己所在的目录」读
meow-desk.exe 的，分开放就装不了。
"""

import datetime
import os
import shutil
import subprocess
import sys
import zipfile

HERE = os.path.dirname(os.path.abspath(__file__))
CORE = os.path.dirname(HERE)
BUILD = os.path.join(CORE, "build")
DIST = os.path.join(CORE, "dist")
STAGE = os.path.join(DIST, "package")

README = """meow-desk 安装说明
==============================

版本：{ver}

怎么装
------
1. 把 meow-setup.exe 和 meow-desk.exe 放在同一个文件夹里（本压缩包解开就是这样，
   别只把 setup 单独拖出来）。
2. 双击 meow-setup.exe，选安装位置，再选数据放哪儿：
     · 默认：%APPDATA%\\meow-desk（跟着用户走，重装不丢）
     · 便携：exe 同目录（U 盘带着走）
3. 点「安装」，开始菜单和桌面上会各建一个 meow-desk 快捷方式。

默认装到
--------
%LOCALAPPDATA%\\Programs\\meow-desk，不需要管理员权限。

卸载
----
安装器只做「复制程序 + 建快捷方式」，没有注册系统卸载入口，手动删这几样就行：
  1. 安装目录（默认 %LOCALAPPDATA%\\Programs\\meow-desk）
  2. 开始菜单和桌面上的 meow-desk 快捷方式
  3. 想连布局记忆一起清掉，再删 %APPDATA%\\meow-desk
     （便携模式删安装目录里的 meow-desk.db 和 meow-desk.portable）

注意
----
meow-setup.exe 是从「它自己所在的目录」读 meow-desk.exe 的，两个文件分开就装不了。
"""


def run(args):
    return subprocess.run([sys.executable] + args, cwd=CORE).returncode


def version():
    date = datetime.date.today().strftime("%Y.%m.%d")
    try:
        p = subprocess.run(["git", "rev-parse", "--short", "HEAD"], cwd=CORE,
                           stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, text=True)
        h = p.stdout.strip() if p.returncode == 0 else ""
    except OSError:
        h = ""
    return date + "-" + (h or "nogit")


def main(argv):
    do_build = "--no-build" not in argv
    do_zip = "--no-zip" not in argv
    do_verify = "--no-verify" not in argv
    if "-h" in argv or "--help" in argv:
        print(__doc__)
        return 0

    if do_build:
        print("== 1/4 编译 ==")
        if run([os.path.join(HERE, "build.py")]) != 0:
            return 1
    else:
        print("== 1/4 编译（--no-build，跳过）==")

    for name in ("meow-desk.exe", "meow-setup.exe"):
        p = os.path.join(BUILD, name)
        if not os.path.exists(p):
            print("缺少 %s —— 先编译一次（去掉 --no-build）" % p)
            return 1

    if do_verify:
        print("")
        print("== 2/4 图标自检 ==")
        if run([os.path.join(HERE, "verify_icon.py")]) != 0:
            print("自检没过：exe 里的图标与 res/meow-desk.ico 不一致，"
                  "先跑 set-icon 重做图标")
            return 1
    else:
        print("")
        print("== 2/4 图标自检（跳过）==")

    ver = version()
    zip_path = os.path.join(DIST, "MeowDesk-%s.zip" % ver)
    root = "MeowDesk-%s" % ver

    print("")
    print("== 3/4 组装 dist/（版本 %s）==" % ver)
    os.makedirs(DIST, exist_ok=True)
    # 老位置：平铺一份，以前直接跑 dist/meow-setup.exe 的用法不能坏
    for name in ("meow-desk.exe", "meow-setup.exe"):
        shutil.copy2(os.path.join(BUILD, name), os.path.join(DIST, name))

    # 暂存目录固定名字：带版本号的目录每提交一次多一个，几天就成垃圾堆
    if os.path.isdir(STAGE):
        shutil.rmtree(STAGE)
    os.makedirs(STAGE)
    for name in ("meow-desk.exe", "meow-setup.exe"):
        shutil.copy2(os.path.join(BUILD, name), os.path.join(STAGE, name))
    # UTF-8 BOM + CRLF：记事本、写字板打开都不乱码
    with open(os.path.join(STAGE, "安装说明.txt"), "w",
              encoding="utf-8-sig", newline="\r\n") as f:
        f.write(README.format(ver=ver))

    print("  dist/package/")
    for name in sorted(os.listdir(STAGE)):
        print("    " + name)

    if do_zip:
        print("")
        print("== 4/4 压 zip ==")
        with zipfile.ZipFile(zip_path, "w", zipfile.ZIP_DEFLATED) as z:
            for name in sorted(os.listdir(STAGE)):
                p = os.path.join(STAGE, name)
                if os.path.isfile(p):
                    z.write(p, root + "/" + name)
        print("  %s  (%.1f MB)" % (os.path.relpath(zip_path, CORE),
                                   os.path.getsize(zip_path) / 1048576.0))

    print("")
    print("完成。")
    print("  分发用：%s" % (os.path.relpath(zip_path, CORE) if do_zip else "dist/package/"))
    print("  本机试用：先退出正在运行的 meow-desk，再启动 dist/meow-desk.exe")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))

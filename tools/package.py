#!/usr/bin/env python3
"""打包成可分发的安装包（一条命令出 zip）。

用法（三种终端都能跑，见同目录的 make-setup.sh / make-setup.bat）：
    make-setup.bat              <- cmd / PowerShell（双击也行）
    ./make-setup.sh             <- Git Bash
    python tools/package.py

选项：
    --version 1.2.0   指定版本号（写回 version.txt，下次打包继续用它）
    --no-build    不编译，直接拿 build/ 里现成的 exe 打包
    --no-zip      只出目录，不压 zip
    --no-verify   跳过资源自检（exe 图标 / 版本信息）

版本号：仓库根的 version.txt（一行，默认 0.1.0）。不带参数就用它，
产物名字会带上：dist/MeowDesk_v<版本>.zip，包内根目录 MeowDesk_v<版本>/。

产物：
    dist/MeowDesk.exe + dist/MeowDesk_v<版本>_setup.exe   平铺一份（老习惯，直接能跑安装程序）
    dist/package/                               分发目录（两个 exe + 安装说明.txt + version.txt）
    dist/MeowDesk_v<版本>.zip                   发给别人就发这个

命名（全套统一，别再各写各的）：安装器 MeowDesk_v0.1.0_setup.exe、主程序
MeowDesk.exe、zip 与包内目录 MeowDesk_v0.1.0。这些名字是用户定的，改之前先问。

主程序已经编进安装器（IDR_PAYLOAD），所以 setup 单独拷走也能装；
包里再放一份 MeowDesk.exe 是为了「不想安装、直接双击就跑」。
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
# 版本号：仓库根的 version.txt。用 --version 改它会顺手写回去，
# 所以「改一次版本号」只要打一次 --version，后面直接打包就是新版本。
VERSION_FILE = os.path.join(CORE, "version.txt")
DEFAULT_VERSION = "0.1.0"

README = """MeowDesk 安装说明
==============================

版本：{ver}（{commit}）

包里的两个文件
--------------
  1) {setup}
     安装程序，双击这个。主程序已经打进它里面了，
     单独把这一个文件拷到哪儿都能装（从 zip 里直接双击也行）。
  2) MeowDesk.exe
     免安装版：不想装就直接双击它，可以放在任意文件夹。

怎么装
------
1. 双击 {setup}。
2. 选安装位置；再选数据放哪儿：
     · 默认：%APPDATA%\\meow-desk（跟着用户走，重装不丢）
     · 便携：程序同目录（U 盘带着走）
   已经装过的话，这里会直接填好上次的位置，点「重新安装」即可。
3. 点「安装」，开始菜单和桌面上会各建一个 MeowDesk 快捷方式，指向 MeowDesk.exe。

默认装到
--------
%LOCALAPPDATA%\\Programs\\meow-desk，不需要管理员权限。

卸载
----
安装器只做「复制程序 + 建快捷方式」，没有注册系统卸载入口，手动删这几样就行：
  1. 安装目录（默认 %LOCALAPPDATA%\\Programs\\meow-desk，里面是 MeowDesk.exe）
  2. 开始菜单和桌面上的 MeowDesk 快捷方式
  3. 想连布局记忆一起清掉，再删 %APPDATA%\\meow-desk
     （便携模式删安装目录里的 meow-desk.db 和 meow-desk.portable）
"""


def run(args):
    return subprocess.run([sys.executable] + args, cwd=CORE).returncode


def git_short():
    try:
        p = subprocess.run(["git", "rev-parse", "--short", "HEAD"], cwd=CORE,
                           stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, text=True)
        return p.stdout.strip() if p.returncode == 0 else ""
    except OSError:
        return ""


def read_version():
    """版本号存在仓库根的 version.txt 里（一行，比如 0.2.0）。"""
    try:
        with open(VERSION_FILE, encoding="utf-8") as f:
            v = f.readline().strip()
    except OSError:
        v = ""
    return v or DEFAULT_VERSION


def write_version(v):
    with open(VERSION_FILE, "w", encoding="utf-8", newline="\n") as f:
        f.write(v + "\n")


def arg_value(argv, names):
    """取 --version 1.2.3 / --version=1.2.3 / -v 1.2.3 的值。"""
    for i, a in enumerate(argv):
        for n in names:
            if a == n and i + 1 < len(argv):
                return argv[i + 1]
            if a.startswith(n + "="):
                return a.split("=", 1)[1]
    return None


def main(argv):
    do_build = "--no-build" not in argv
    do_zip = "--no-zip" not in argv
    do_verify = "--no-verify" not in argv
    if "-h" in argv or "--help" in argv:
        print(__doc__)
        return 0

    # 版本号：--version 给了就用它并写回 version.txt（下次直接打包就是这个版本）
    v = arg_value(argv, ("--version", "-v"))
    if v:
        v = v.strip().lstrip("vV")
        if not v:
            print("--version 后面要跟版本号，比如 --version 1.2.0")
            return 1
        write_version(v)
        print("版本号已设为 %s（已写回 version.txt）" % v)
    ver = read_version()
    commit = git_short()

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
        print("== 2/4 资源自检 ==")
        if run([os.path.join(HERE, "verify_icon.py")]) != 0:
            print("自检没过：exe 里的图标与 res/meow-desk.ico 不一致，"
                  "先跑 set-icon 重做图标")
            return 1
        # 版本资源同样是「看不见摸不着」：缺了编译器不会报错，
        # 只有用户右键属性时才发现全是空的，所以打包前必须读回来验一遍
        if run([os.path.join(HERE, "verify_version.py")]) != 0:
            print("自检没过：exe 缺少版本信息，或版本号与 version.txt 不一致。"
                  "删掉 build/ 重新 configure 编译一次")
            return 1
    else:
        print("")
        print("== 2/4 资源自检（跳过）==")

    # 命名全用一套：MeowDesk_v<版本>（zip / 包内目录 / 安装器）+ MeowDesk.exe（主程序）
    zip_path = os.path.join(DIST, "MeowDesk_v%s.zip" % ver)
    root = "MeowDesk_v%s" % ver
    # 包里的文件名：安装器带版本号，主程序固定叫 MeowDesk.exe
    setup_name = "MeowDesk_v%s_setup.exe" % ver
    files = ((os.path.join(BUILD, "meow-desk.exe"), "MeowDesk.exe"),
             (os.path.join(BUILD, "meow-setup.exe"), setup_name))

    print("")
    print("== 3/4 组装 dist/（版本 %s%s）==" % (ver, (" / " + commit) if commit else ""))
    os.makedirs(DIST, exist_ok=True)
    # 老位置：平铺一份，以前直接跑 dist/ 里 setup 的用法不能坏。
    # 顺便把改名前 / 旧版本号的产物清掉，否则目录里会出现两套、分不清哪个是新的。
    # 注意 Windows 文件名大小写不敏感：按小写比较，别把这次要生成的删了。
    want = {n.lower() for _, n in files}
    stale = lambda n: (n.lower() not in want) and (
        n.lower() in ("meow-desk.exe", "meow-setup.exe", "meowplayer.exe")
        or (n.startswith("MeowDesk_v") and n.endswith("_setup.exe")))
    for name in os.listdir(DIST):
        if not stale(name):
            continue
        try:
            os.remove(os.path.join(DIST, name))
        except OSError:
            pass
    for src, name in files:
        shutil.copy2(src, os.path.join(DIST, name))

    # 暂存目录固定名字：带版本号的目录每提交一次多一个，几天就成垃圾堆
    if os.path.isdir(STAGE):
        shutil.rmtree(STAGE)
    os.makedirs(STAGE)
    for src, name in files:
        shutil.copy2(src, os.path.join(STAGE, name))
    # 安装程序会读「它自己旁边的 version.txt」显示版本号，所以两个位置都放一份
    for d in (DIST, STAGE):
        with open(os.path.join(d, "version.txt"), "w", encoding="ascii", newline="\n") as f:
            f.write(ver + "\n")
    # UTF-8 BOM + CRLF：记事本、写字板打开都不乱码
    with open(os.path.join(STAGE, "安装说明.txt"), "w",
              encoding="utf-8-sig", newline="\r\n") as f:
        f.write(README.format(ver=ver, commit=commit or "nogit", setup=setup_name))

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
    print("  本机试用：先退出正在运行的 MeowDesk，再启动 dist/MeowDesk.exe")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))

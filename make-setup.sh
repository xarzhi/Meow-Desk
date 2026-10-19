#!/bin/bash
# 打包成可分发的安装包（一条命令出 zip）：
#
#   ./make-setup.sh               编译 → 组装 dist/package/ → 压 zip
#   ./make-setup.sh --no-build    不编译，直接拿 build/ 里现成的 exe 打包
#   ./make-setup.sh --no-zip      只出目录，不压 zip
#   ./make-setup.sh --no-verify   跳过图标自检
#
# 产物：
#   dist/meow-desk.exe + dist/meow-setup.exe   平铺一份（原地跑安装程序的旧习惯）
#   dist/package/                               分发目录（两个 exe + 安装说明）
#   dist/MeowDesk-<版本>.zip                    发给别人就发这个
#
# 为什么两个 exe 必须放一起：meow-setup.exe 是从「它自己所在的目录」读
# meow-desk.exe 的，分开放就装不了（setup.cpp 里会弹「找不到 meow-desk.exe」）。
set -u

cd "$(dirname "$0")"
CORE="$(pwd)"

DO_BUILD=1
DO_ZIP=1
DO_VERIFY=1

while [ $# -gt 0 ]; do
  case "$1" in
    --no-build)  DO_BUILD=0 ;;
    --no-zip)    DO_ZIP=0 ;;
    --no-verify) DO_VERIFY=0 ;;
    -h|--help)   awk 'NR>1 && /^#/ {sub(/^# ?/,""); print; next} NR>1 {exit}' "$0"; exit 0 ;;
    *)           echo "不认识的选项：$1（--help 看用法）"; exit 2 ;;
  esac
  shift
done

find_python() {
  for c in python3 python py; do
    if command -v "$c" >/dev/null 2>&1; then command -v "$c"; return 0; fi
  done
  for p in \
    "/c/Users/22357/.workbuddy/binaries/python/versions/3.13.12/python.exe" \
    "/c/Python313/python.exe" \
    "/c/Python312/python.exe" \
    "/c/Program Files/Python313/python.exe"; do
    if [ -x "$p" ]; then echo "$p"; return 0; fi
  done
  return 1
}
PY="$(find_python)" || PY=""

if [ "$DO_BUILD" -eq 1 ]; then
  echo "== 1/4 编译 =="
  tools/run_build.sh || exit 1
else
  echo "== 1/4 编译（--no-build，跳过）=="
fi

for f in build/meow-desk.exe build/meow-setup.exe; do
  if [ ! -f "$f" ]; then
    echo "缺少 $f —— 先跑 ./build.sh（或去掉 --no-build）"
    exit 1
  fi
done

if [ "$DO_VERIFY" -eq 1 ] && [ -n "$PY" ]; then
  echo ""
  echo "== 2/4 图标自检 =="
  "$PY" tools/verify_icon.py || {
    echo "自检没过：exe 里的图标与 res/meow-desk.ico 不一致，先 ./set-icon.sh 重做图标"
    exit 1
  }
else
  echo ""
  echo "== 2/4 图标自检（跳过）=="
fi

# 版本号：日期 + 短 commit（仓库还没打 tag，有 tag 就换成 tag）
DATE="$(date +%Y.%m.%d)"
HASH="$(git rev-parse --short HEAD 2>/dev/null || echo nogit)"
VER="$DATE-$HASH"
[ -n "$VER" ] || { echo "版本号算空了，中止"; exit 1; }

# 暂存目录用固定名字：每打一次包就多一个带版本号的目录，几天就堆成垃圾堆。
# 版本号只体现在 zip 名上（zip 里那层根目录才是带版本号的）。
OUT="dist/package"
ZIP="dist/MeowDesk-$VER.zip"

echo ""
echo "== 3/4 组装 dist/（版本 $VER）=="
mkdir -p dist

# 旧位置：平铺一份，之前习惯直接跑 dist/meow-setup.exe 的用法不能坏
cp -f build/meow-desk.exe build/meow-setup.exe dist/

# 暂存目录每次重建，保证不带上一次的残留。用 python 删不走 shell 的 rm。
if [ -d "$OUT" ] && [ -n "$PY" ]; then
  "$PY" -c "import shutil,sys; shutil.rmtree(sys.argv[1])" "$OUT"
fi
mkdir -p "$OUT"
cp -f build/meow-desk.exe "$OUT/"
cp -f build/meow-setup.exe "$OUT/"

# 安装说明用 UTF-8 BOM 写：Win11 记事本、老版写字板打开都不会乱码
if [ -n "$PY" ]; then
  "$PY" - "$OUT/安装说明.txt" "$VER" <<'PY'
import sys
path, ver = sys.argv[1], sys.argv[2]
text = """meow-desk 安装说明
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
""".format(ver=ver)
with open(path, "w", encoding="utf-8-sig", newline="\r\n") as f:
    f.write(text)
PY
fi

echo "  $OUT/"
ls -1 "$OUT" | sed 's/^/    /'

if [ "$DO_ZIP" -eq 1 ]; then
  echo ""
  echo "== 4/4 压 zip =="
  if [ -z "$PY" ]; then
    echo "没有 python，跳过 zip；直接发 $OUT/ 这个文件夹也行"
    DO_ZIP=0
  else
    "$PY" - "$OUT" "$ZIP" "MeowDesk-$VER" <<'PY'
import os, sys, zipfile
outdir, zippath, root = sys.argv[1], sys.argv[2], sys.argv[3]
with zipfile.ZipFile(zippath, "w", zipfile.ZIP_DEFLATED) as z:
    for name in sorted(os.listdir(outdir)):
        p = os.path.join(outdir, name)
        if os.path.isfile(p):
            z.write(p, root + "/" + name)
print("  %s  (%.1f MB)" % (zippath, os.path.getsize(zippath) / 1048576.0))
PY
  fi
fi

echo ""
echo "完成。"
echo "  分发用：$( [ "$DO_ZIP" -eq 1 ] && echo "$ZIP" || echo "$OUT/" )"
echo "  本机试用：先退出正在运行的 meow-desk，再启动 dist/meow-desk.exe"

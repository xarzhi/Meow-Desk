#!/bin/bash
# 换程序图标：一张图进去，exe / 任务栏 / 托盘 / 关于页的图标全换掉。
#
# 用法：
#   ./set-icon.sh 图片.png        换图 + 生成 + 编译 + 自检（一条命令搞定）
#   ./set-icon.sh                 不换图，用当前 logo.png 重新生成 + 编译
#   ./set-icon.sh 图片.jpg        非 PNG 也行，会先转成 PNG 再换
#
# 选项：
#   --no-build    只生成 res/ 里的图标资源，不编译（下次 build 时生效）
#   --no-verify   跳过「exe 里的图标和 ico 是否一致」的校验
#   -h, --help    看这段说明
#
# 生成的资源：
#   res/meow-desk.ico     exe 图标 + 托盘 + 任务栏 + 窗口图标（9 个尺寸）
#   res/logo-128.png      关于页里的 logo
#   res/logo-256.png      备用大图
# 旧 logo.png 会被备份到 build/logo.prev.<时间>.png，仓库里只留新图。
#
# 注意：图是**整张**缩放的，不裁、不挑其中一块、不重绘。
set -u

cd "$(dirname "$0")"
CORE="$(pwd)"

IMG=""
DO_BUILD=1
DO_VERIFY=1

while [ $# -gt 0 ]; do
  case "$1" in
    --no-build)  DO_BUILD=0 ;;
    --no-verify) DO_VERIFY=0 ;;
    -h|--help)   awk 'NR>1 && /^#/ {sub(/^# ?/,""); print; next} NR>1 {exit}' "$0"; exit 0 ;;
    -*)          echo "不认识的选项：$1（--help 看用法）"; exit 2 ;;
    *)           if [ -n "$IMG" ]; then echo "只接受一张图片"; exit 2; fi; IMG="$1" ;;
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
    "/c/Program Files/Python313/python.exe" \
    "/c/Program Files/Python312/python.exe"; do
    if [ -x "$p" ]; then echo "$p"; return 0; fi
  done
  return 1
}

PY="$(find_python)" || { echo "找不到 python，装一个或者把路径写进本脚本"; exit 1; }

echo "== 1/3 生成图标资源 =="
if [ -n "$IMG" ]; then
  if [ ! -f "$IMG" ]; then echo "图片不存在：$IMG"; exit 1; fi
  "$PY" tools/make_icon.py --install "$IMG" --no-build-hint || exit 1
else
  "$PY" tools/make_icon.py --no-build-hint || exit 1
fi

if [ "$DO_BUILD" -eq 0 ]; then
  echo ""
  echo "--no-build：资源已更新，但没有编译。跑 ./build.sh 或 ./set-icon.sh 才会进 exe。"
  exit 0
fi

echo ""
echo "== 2/3 编译 =="
tools/run_build.sh || exit 1

if [ "$DO_VERIFY" -eq 1 ]; then
  echo ""
  echo "== 3/3 自检：exe 里的图标是不是刚生成的那一份 =="
  "$PY" tools/verify_icon.py || {
    echo ""
    echo "自检没过：exe 里的图标和 res/meow-desk.ico 对不上。"
    echo "通常是编译产物不是这一份，或者跑的是别的目录里的旧 exe。"
    exit 1
  }
fi

echo ""
echo "完成。图标已换："
echo "  exe 图标 / 任务栏 / 托盘 / 关于页 —— 全部来自 res/meow-desk.ico"
echo ""
echo "看效果前**先退出正在运行的 meow-desk**：它还占着旧图标和旧代码，"
echo "重新启动 build/meow-desk.exe 才会变。分发就再跑 ./make-setup.sh。"

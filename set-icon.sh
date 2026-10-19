#!/bin/bash
# 换程序图标（Git Bash 入口）。真正的逻辑在 tools/icon.py，
# 所以 cmd / PowerShell 里用 set-icon.bat 效果完全一样。
#
#   ./set-icon.sh 图片.png     换图 + 生成 + 编译 + 自检
#   ./set-icon.sh              不换图，用当前 logo.png 重新生成 + 编译
#   ./set-icon.sh --help       完整说明
set -u
cd "$(dirname "$0")"

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

PY="$(find_python)" || { echo "找不到 python，装一个或者把路径写进本脚本"; exit 1; }
exec "$PY" tools/icon.py "$@"

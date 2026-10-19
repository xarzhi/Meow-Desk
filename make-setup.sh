#!/bin/bash
# 打包成可分发的安装包（Git Bash 入口）。真正的逻辑在 tools/package.py，
# 所以 cmd / PowerShell 里用 make-setup.bat 效果完全一样。
#
#   ./make-setup.sh              编译 → dist/package/ → zip
#   ./make-setup.sh --version 1.2.0   指定版本号（写回 version.txt，下次继续用）
#   ./make-setup.sh --no-build   不编译，用 build/ 里现成的 exe 打包
#   ./make-setup.sh --help       完整说明
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
exec "$PY" tools/package.py "$@"

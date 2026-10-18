#!/bin/bash
# 打包分发：setup.exe 是从「自己所在目录」读 meow-desk.exe 的，
# 所以两个文件必须放在一起。
set -e
cd "$(dirname "$0")"

if [ ! -f build/meow-setup.exe ] || [ ! -f build/meow-desk.exe ]; then
  echo "先跑 ./build.sh"
  exit 1
fi

mkdir -p dist
cp build/meow-desk.exe build/meow-setup.exe dist/
echo "已打包到 dist/："
ls -lh dist/
echo ""
echo "分发时把 dist/ 里的两个文件放在同一个文件夹，运行 meow-setup.exe 即可。"

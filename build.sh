#!/bin/bash
# meow-desk-core 构建脚本（用 toolchain 文件固化 MSVC 路径，不依赖 vcvars64）
set -e

export NINJA="/c/Program Files/Microsoft Visual Studio/18/Community/Common7/IDE/CommonExtensions/Microsoft/CMake/Ninja/ninja.exe"

cd "$(dirname "$0")"
mkdir -p build
cd build

cmake -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE="../msvc-toolchain.cmake" \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_MAKE_PROGRAM="$NINJA" \
  ..

cmake --build . --config Release

echo ""
echo "Build done: build/meow-desk.exe"

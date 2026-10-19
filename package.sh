#!/bin/bash
# 旧名字保留（肌肉记忆 / 以前的命令还能用）：打包现在统一走 make-setup.sh。
# 它会编译、自检图标、出 dist/MeowDesk-<版本>/ 并压成 zip。
#
# 想跳过编译（直接用 build/ 里现成的 exe）：./package.sh --no-build
set -u
exec "$(dirname "$0")/make-setup.sh" "$@"

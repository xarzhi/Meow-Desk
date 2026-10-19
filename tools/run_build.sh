#!/bin/bash
# 跑一次 ./build.sh，只把关键几行打出来；失败时给出人话原因。
#
# 最常见的失败是 LNK1104：meow-desk.exe 正被运行中的程序占着。
# 这里不杀进程，只提示先退出程序。
#
# 完整日志留在 build/build.log（build/ 是忽略目录），失败后可以直接翻。
# 不用 mktemp/rm：临时文件删不删无所谓，留一份日志反而有用。
#
# 返回：0 成功，1 失败
set -u

CORE="$(cd "$(dirname "$0")/.." && pwd)"
LOG="$CORE/build/build.log"

mkdir -p "$CORE/build"
: > "$LOG"

if "$CORE/build.sh" >"$LOG" 2>&1; then
  grep -E "Build done" "$LOG" | tail -1
  exit 0
fi

echo "---- 编译失败（完整日志：build/build.log）----"
tail -40 "$LOG"
echo ""
if grep -q "LNK1104" "$LOG"; then
  echo "→ build/meow-desk.exe 被占用：meow-desk 还开着。"
  echo "  退出程序后再跑一次（脚本不替你杀进程，也不改产物名）。"
elif grep -q "C1083\|C1034" "$LOG"; then
  echo "→ 头文件/环境问题：确认 msvc-toolchain.cmake 里的 VS / SDK 路径还对不对。"
fi
exit 1

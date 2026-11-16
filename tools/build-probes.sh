#!/usr/bin/env bash
# 交叉编译 nl80211 探针工具（不需要 Android NDK，只需要 zig）
#   brew install zig
set -euo pipefail
cd "$(dirname "$0")"
for t in nlprobe rawtx delif; do
    zig cc -target aarch64-linux-musl -Os -static "$t.c" -o "$t"
    echo "built $t ($(stat -c%s "$t" 2>/dev/null || stat -f%z "$t") bytes)"
done

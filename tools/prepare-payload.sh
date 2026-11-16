#!/usr/bin/env bash
# 把 Quick Share extension（com.google.android.mosey）APK 放进模块目录
#
# 用法: ./tools/prepare-payload.sh /path/to/MoseyApp.apk
#
# 合法来源：
#   1. Pixel 9 / Pixel 10 系统镜像中的 /system_ext/priv-app/MoseyApp/MoseyApp.apk（推荐，原厂系统签名）；
#   2. APKMirror 搜索 "Quick Share extension"（注意只用来测试，别传播）。
# 本仓库不附带该 APK（版权归 Google）。
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
SRC="${1:-}"
[ -n "$SRC" ] && [ -f "$SRC" ] || { echo "用法: $0 /path/to/MoseyApp.apk" >&2; exit 1; }

size=$(wc -c < "$SRC" | tr -d ' ')
echo "输入: $SRC ($size bytes)"
if [ "$size" -lt 20000000 ] || [ "$size" -gt 45000000 ]; then
  echo "警告: 大小不像 Pixel 系统镜像版（约 26.9 MB）；APKMirror 的拆分版需要合并后再用" >&2
fi

DST="$ROOT/system_ext/priv-app/MoseyApp/MoseyApp.apk"
cp "$SRC" "$DST"
chmod 0644 "$DST"
echo "已写入: $DST"
echo "sha256: $(shasum -a 256 "$DST" | cut -d' ' -f1)"
echo
echo "接下来: ./tools/build.sh   # 生成可刷入的模块 zip"

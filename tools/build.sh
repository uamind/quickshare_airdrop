#!/usr/bin/env bash
# 打包 KernelSU 模块 zip（需要先准备 payload，见 tools/prepare-payload.sh）
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
APK="$ROOT/system_ext/priv-app/MoseyApp/MoseyApp.apk"
VER="$(sed -n 's/^version=//p' "$ROOT/module.prop")"
OUT="${1:-$ROOT/dist/quickshare_airdrop-${VER}.zip}"

[ -f "$APK" ] || { echo "缺少 payload: $APK" >&2; echo "先运行: tools/prepare-payload.sh <MoseyApp.apk>" >&2; exit 1; }

size=$(wc -c < "$APK" | tr -d ' ')
echo "payload : $size bytes"
echo "sha256  : $(shasum -a 256 "$APK" | cut -d' ' -f1)"
mkdir -p "$(dirname "$OUT")"
rm -f "$OUT"
(cd "$ROOT" && zip -r -X "$OUT" module.prop customize.sh service.sh uninstall.sh README.md system_ext >/dev/null)
echo "生成    : $OUT"
unzip -l "$OUT" | tail -2

#!/system/bin/sh
# Quick Share AirDrop 现场注入（仅用于临时 root，不写系统分区）
# 用法：adb push MoseyApp.apk /data/local/tmp/ 后
#       su -c "sh /data/local/tmp/qsa-inject.sh"
#
# 原理：Android 17 上 Mosey 必须是系统应用（FLAG_SYSTEM）才能被三星 ShareLive 接受，
# 但临时 root 时 KernelSU 模块不会在开机阶段挂载，所以把 Mosey 塞进
# /system_ext/priv-app 下一个现有目录的“影子目录”，再软重启框架让 PackageManager 重新扫描。
# 注意：PackageManager 要求“一个系统 app 目录只能有一个包”，所以影子目录里只放 Mosey，
# 目标目录原来的 APK 被遮蔽（真重启后挂载消失，原 App 自动回来）。
set -u

LOG=/data/local/tmp/qsa-inject.log
exec >>"$LOG" 2>&1
echo "=== $(date) inject start ==="

SLOT=/data/adb/qsa_slot
APPDIR=/system_ext/priv-app/GoogleFeedback
PAYLOAD=/data/local/tmp/MoseyApp.apk

[ "$(id -u)" = "0" ] || { echo "! 需要 root"; exit 1; }
[ -f "$PAYLOAD" ] || { echo "! 缺少 $PAYLOAD"; exit 1; }
[ -f "$APPDIR/GoogleFeedback.apk" ] || { echo "! 目标目录不完整：$APPDIR"; exit 1; }

# 清理上一次的挂载（目录挂载 + 旧版遗留的文件挂载），再重建 slot
unmount_all() {
  nsenter -t 1 -m -- sh -c "
    for i in 1 2 3; do
      umount $APPDIR 2>/dev/null || true
      umount $SLOT/GoogleFeedback.apk 2>/dev/null || true
      umount $SLOT/MoseyApp.apk 2>/dev/null || true
    done
  " >/dev/null 2>&1 || true
}
unmount_all
rm -rf "$SLOT"
mkdir -p "$SLOT"

# 关键：一个系统 app 目录只能有一个包，否则 PM 报
#   "Inconsistent package com.google.android.feedback ...; expected com.google.android.mosey"
# 而整个目录扫描作废。所以 slot 里只放 Mosey，目标目录原来的 APK 被遮蔽（真重启后自动恢复）。
cp "$PAYLOAD" "$SLOT/MoseyApp.apk" || exit 1
chmod 0644 "$SLOT/MoseyApp.apk"
chcon u:object_r:system_file:s0 "$SLOT" 2>/dev/null || true
chcon u:object_r:system_file:s0 "$SLOT/MoseyApp.apk" 2>/dev/null || true

echo "--- slot:"
ls -lZ "$SLOT"

if ! nsenter -t 1 -m -- mount --bind "$SLOT" "$APPDIR"; then
  echo "! mount 失败"
  exit 1
fi
echo "--- mounted:"
ls -l "$APPDIR"
grep -F " $APPDIR " /proc/1/mounts | tail -2

# 临时放宽特权权限白名单校验：我们没把 XML 挂进 /system_ext/etc/permissions
resetprop ro.control_privapp_permissions disable
echo "privapp: $(getprop ro.control_privapp_permissions)"

echo "=== inject done, 需要软重启框架 ==="

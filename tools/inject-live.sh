#!/usr/bin/env bash
# 在临时 root 的 S23U 上现场注入 Mosey（免装模块、免真重启），并软重启框架让 PM 重新扫描。
#
#   ./tools/inject-live.sh              # 注入 + 软重启 + 验证
#   ./tools/inject-live.sh --no-reboot  # 只注入，不重启
#   ./tools/inject-live.sh --undo       # 撤销注入（卸挂载、恢复 privapp 校验）+ 软重启
#
# 环境变量：SERIAL=<adb 序列号> 可指定设备。
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
MOD="$(cd "$HERE/.." && pwd)"
APK="$MOD/system_ext/priv-app/MoseyApp/MoseyApp.apk"

ADB=(adb)
[ -n "${SERIAL:-}" ] && ADB=(adb -s "$SERIAL")

say() { printf '\n\033[1m%s\033[0m\n' "$*"; }

soft_reboot_and_wait() {
  local old_pid new_pid i
  old_pid="$("${ADB[@]}" shell pidof system_server 2>/dev/null | tr -d '\r' || true)"
  say "软重启框架（system_server 旧 pid=${old_pid:-?}）"

  cat > /tmp/qsa-soft-reboot.sh <<'EOF'
#!/system/bin/sh
echo "start $(date)" > /data/local/tmp/qsa-soft-reboot.log
ksud soft-reboot >> /data/local/tmp/qsa-soft-reboot.log 2>&1
echo "exit=$? $(date)" >> /data/local/tmp/qsa-soft-reboot.log
EOF
  "${ADB[@]}" push /tmp/qsa-soft-reboot.sh /data/local/tmp/qsa-soft-reboot.sh >/dev/null
  "${ADB[@]}" shell 'su -c "chmod 755 /data/local/tmp/qsa-soft-reboot.sh; setsid /data/local/tmp/qsa-soft-reboot.sh </dev/null >/dev/null 2>&1 &"'

  for i in $(seq 1 120); do
    sleep 2
    new_pid="$("${ADB[@]}" shell pidof system_server 2>/dev/null | tr -d '\r' || true)"
    if [ -n "$new_pid" ] && [ "$new_pid" != "$old_pid" ]; then
      say "框架已重启：system_server pid=$new_pid"
      return 0
    fi
  done
  echo "! 等待 system_server 重启超时" >&2
  return 1
}

verify() {
  local i path
  say "验证 PackageManager 是否扫描到 Mosey"
  for i in $(seq 1 60); do
    path="$("${ADB[@]}" shell 'su -c "pm path com.google.android.mosey"' 2>/dev/null | tr -d '\r' || true)"
    [ -n "$path" ] && break
    sleep 2
  done
  if [ -z "$path" ]; then
    echo "! 仍未安装 com.google.android.mosey"
    echo "--- 最近的包扫描日志："
    "${ADB[@]}" logcat -d -b all 2>/dev/null | grep -iE "mosey|GoogleFeedback|Failed to parse|scanPackage" | grep -v adbd | tail -30 || true
    return 1
  fi
  echo "  $path"
  "${ADB[@]}" shell 'su -c "dumpsys package com.google.android.mosey | grep -E \"versionName|versionCode|pkgFlags|codePath\" | head -6"' | sed 's/^/  /'
}

do_undo() {
  say "撤销：卸载挂载 + 恢复 privapp 校验"
  "${ADB[@]}" shell 'su -c "
    nsenter -t 1 -m -- umount /system_ext/priv-app/GoogleFeedback 2>/dev/null
    resetprop ro.control_privapp_permissions enforce
    getprop ro.control_privapp_permissions
    rm -rf /data/adb/qsa_slot /data/local/tmp/MoseyApp.apk
  "'
  soft_reboot_and_wait || true
}

case "${1:-}" in
  --undo)
    do_undo
    exit 0
    ;;
  --no-reboot)
    REBOOT=0
    ;;
  "")
    REBOOT=1
    ;;
  *)
    echo "用法: $0 [--no-reboot|--undo]" >&2
    exit 2
    ;;
esac

say "推送 payload 与注入脚本"
"${ADB[@]}" push "$APK" /data/local/tmp/MoseyApp.apk >/dev/null
"${ADB[@]}" push "$HERE/qsa-inject.sh" /data/local/tmp/qsa-inject.sh >/dev/null

say "现场注入（root）"
"${ADB[@]}" shell 'su -c "sh /data/local/tmp/qsa-inject.sh"' | tail -25

grant_perms() {
  say "给 Mosey 预授权限"
  "${ADB[@]}" shell 'su -c "
    for p in \
      android.permission.ACCESS_FINE_LOCATION \
      android.permission.ACCESS_COARSE_LOCATION \
      android.permission.ACCESS_BACKGROUND_LOCATION \
      android.permission.ACCESS_LOCAL_NETWORK \
      android.permission.READ_MEDIA_VISUAL_USER_SELECTED \
      android.permission.NEARBY_WIFI_DEVICES \
      android.permission.BLUETOOTH_ADVERTISE \
      android.permission.BLUETOOTH_SCAN \
      android.permission.ACCESS_MEDIA_LOCATION \
      android.permission.READ_MEDIA_IMAGES \
      android.permission.READ_MEDIA_VIDEO ; do
      pm grant com.google.android.mosey \$p </dev/null >/dev/null 2>&1
    done
    dumpsys package com.google.android.mosey </dev/null 2>/dev/null | grep -c granted=true
  "'
}

if [ "$REBOOT" = "1" ]; then
  soft_reboot_and_wait
  verify
  grant_perms
fi

say "完成"

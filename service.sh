#!/system/bin/sh
# KernelSU late_start service —— 开机后授予 Mosey 运行时权限并记录状态
LOG=/data/local/tmp/quickshare_airdrop.log
exec >>"$LOG" 2>&1
echo "=== $(date) service start ==="

until [ "$(getprop sys.boot_completed)" = "1" ]; do sleep 2; done

PKG=com.google.android.mosey
i=0
while [ $i -lt 60 ]; do
  if pm path "$PKG" </dev/null >/dev/null 2>&1; then break; fi
  sleep 2
  i=$((i+1))
done

echo "pm path: $(pm path "$PKG" </dev/null 2>&1)"
echo "dumpsys flags: $(dumpsys package "$PKG" </dev/null 2>/dev/null | grep -m1 -E 'flags=|pkgFlags=' )"

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
  out=$(pm grant "$PKG" "$p" </dev/null 2>&1)
  [ -n "$out" ] && echo "grant $p -> $out"
done

echo "grants:"
dumpsys package "$PKG" </dev/null 2>/dev/null | grep -E "granted=true" | grep -E "LOCATION|BLUETOOTH|WIFI|MEDIA|LOCAL_NETWORK" | head -30
echo "=== $(date) service done ==="

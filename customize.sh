#!/system/bin/sh
# KernelSU customize.sh —— 安装时执行
MODDIR="${MODPATH:-$PWD}"
APK="$MODDIR/system_ext/priv-app/MoseyApp/MoseyApp.apk"
XML="$MODDIR/system_ext/etc/permissions/privapp-permissions-com.google.android.mosey.xml"

echo "- Quick Share AirDrop 解锁模块"
if [ ! -f "$APK" ]; then
  echo "! 缺少 system_ext/priv-app/MoseyApp/MoseyApp.apk"
  exit 1
fi
if [ ! -f "$XML" ]; then
  echo "! 缺少 privapp-permissions-com.google.android.mosey.xml"
  exit 1
fi

chmod 0755 "$MODDIR/system_ext" "$MODDIR/system_ext/priv-app" "$MODDIR/system_ext/priv-app/MoseyApp" \
            "$MODDIR/system_ext/etc" "$MODDIR/system_ext/etc/permissions" 2>/dev/null
chmod 0644 "$APK" "$XML" 2>/dev/null

echo "- MoseyApp.apk $(wc -c < "$APK") bytes"
echo "- 安装完成，重启后 Mosey 会以系统应用身份出现"

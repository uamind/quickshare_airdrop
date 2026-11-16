#!/usr/bin/env bash
# AirDrop/Protocol X 诊断：Wi-Fi/蓝牙、Mosey 安装与权限、ShareLive 发现日志
# 用法: SERIAL=xxxx ./tools/diag-airdrop.sh
set -uo pipefail
ADB=(adb)
[ -n "${SERIAL:-}" ] && ADB=(adb -s "$SERIAL")
sec() { printf '\n\033[1m== %s\033[0m\n' "$1"; }

sec "设备"
"${ADB[@]}" shell 'getprop ro.product.model; getprop ro.build.version.release; getprop ro.build.version.sep'
sec "Wi-Fi / 蓝牙开关 (1=on)"
"${ADB[@]}" shell 'settings get global wifi_on; settings get global bluetooth_on'
sec "Wi-Fi 状态"
"${ADB[@]}" shell 'cmd wifi status' 2>/dev/null | head -8
sec "网卡（出现 mosey0 表示 AirDrop 传输中）"
"${ADB[@]}" shell 'ip -o link show' 2>/dev/null | awk -F': ' '{print $2}' | tr '\n' ' '; echo
sec "Mosey 安装"
"${ADB[@]}" shell 'su -c "pm path com.google.android.mosey"'
sec "Mosey 已授权限数"
"${ADB[@]}" shell 'su -c "dumpsys package com.google.android.mosey | grep -c granted=true"'
sec "Mosey 服务（GMS 是否绑上 ExternalSharingService）"
"${ADB[@]}" shell 'su -c "dumpsys activity services com.google.android.mosey"' 2>/dev/null | grep -E 'ServiceRecord|Intent|app=' | head -12
sec "ShareLive 进程"
"${ADB[@]}" shell 'pidof com.samsung.android.app.sharelive'
sec "ShareLive 发现日志"
"${ADB[@]}" shell 'P=$(pidof com.samsung.android.app.sharelive); if [ -n "$P" ]; then logcat -d --pid=$P | grep -iE "protocolx|mosey|discovery|awdl|bluetooth" | tail -60; else echo "(ShareLive 未运行)"; fi'

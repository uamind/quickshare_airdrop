#!/system/bin/sh
# KernelSU uninstall.sh
# 模块卸载后，重启即不再挂载 Mosey；已有的应用数据不会被删除。
# 如果 Play 曾把 Mosey 更新到 /data/app，卸载模块后它可能作为普通应用残留，
# 需要的话自行执行：pm uninstall com.google.android.mosey && pm uninstall -k com.google.android.mosey
echo "- quickshare_airdrop: 重启后系统不再挂载 Mosey，可正常使用 Quick Share（AirDrop 功能会消失）。"

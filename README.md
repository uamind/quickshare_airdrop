# Quick Share AirDrop 解锁

让**三星（及部分其他品牌）的 Quick Share** 用上 Google 只开放给 **Pixel 9 / Pixel 10** 的 **AirDrop 兼容**能力。

原理一句话：Google 把 AirDrop 能力放在独立应用 **Quick Share extension（`com.google.android.mosey`）** 里，
三星 ShareLive（Quick Share）只要发现它「签名合规 + 是系统应用 + 提供 `SHARING_PROVIDER` 服务」就会打开
内部的 **Protocol X** 开关。本模块就是把 Pixel 系统镜像里的原版 Mosey 以系统应用身份装进 `/system_ext`。

> 不改三星 App、不改 GMS、不 hook 代码；用的就是 Google 原版 APK 与其 privapp 白名单。
> 详细逆向过程见 [docs/REVERSE_ENGINEERING.md](docs/REVERSE_ENGINEERING.md)。

## 下载

- 预打包模块（含 Google 原版 Mosey APK，仅供测试，请勿再分发）：[Releases](https://github.com/uamind/quickshare_airdrop/releases)
- 只想自己构建：按下面「安装 A」用 `tools/prepare-payload.sh` + `tools/build.sh`。

## 重要：固件限制（2026-11 实测结论）

在 SM-S9180（One UI 9 / Android 17）上把 ShareLive 侧全部打通（`protocolXSupport=true`、`isProtocolXAvailable=true`、
GMS 也把 Mosey 当外部 provider 绑定并调用 `startDiscovery`）之后，Mosey 自己初始化失败：

```
I Mosey.A : API: start() called with options: DiscoveryOptions(...)
I Mosey.A : Wakeup advertising started ...
E ServiceManager: error in isDeclared
E ServiceManager: java.lang.SecurityException: SELinux denied for service.
I Mosey.A : Mosey Lib is not available.
E Mosey.A : java.lang.IllegalStateException: Mosey service is not available.
```

```
$ su -c "service check Mosey"
Service Mosey: not found
```

也就是说 Google 的 AirDrop 实现依赖一个**系统级 native 服务 `Mosey`**（负责 AWDL/射频那层），它只存在于
Pixel 9/10 的固件里；三星 One UI 没有这个服务，也没有对应的 SELinux service_context，第三方 App/模块无法补出来。
这正是 Google 声明「compatibility supported only by Google」「仅 Pixel 9/10」的真正原因——不是 App 层开关，而是固件能力。

因此本模块的作用边界是：**把 ShareLive 的 Protocol X 开关和 Mosey 扩展装好**；在带 `Mosey` 服务的固件上可直接工作，
在没有该服务的机型（如 S23 Ultra）上，UI 会显示已支持但发现/传输会停在 `Mosey service is not available`。

### 那能不能自己把 AWDL 那层写出来？（实测：不能）

用 `nl80211` 直接问内核，S23 Ultra 的实测结果是：

| 能力 | 结果 |
| --- | --- |
| 建 monitor/radiotap 接口（等价 Pixel 的 `wondertap0`） | ✅ 可以创建 |
| Wi-Fi Aware / NAN（`wifi-aware0`） | ✅ 存在 |
| 设置 monitor 接口信道（AWDL 跳频的前提） | ❌ `NL80211_CMD_SET_WIPHY` 返回 `-EBUSY` |

`-EBUSY` 来自 `cfg80211_set_monitor_channel()`：只有「该 phy 上全是 monitor 接口」才允许设信道，而 Android 的
`wlan0` 永远存在（关掉 Wi-Fi 也一样）。没有信道控制权就没有 AWDL 同步窗口，因此**靠 root/模块补不出 AirDrop**；
Pixel 能做的原因是它的 Wi-Fi 驱动/固件给 `mosey_server` 留了厂商私有通道。完整过程、命令与输出见
[docs/AWDL_FEASIBILITY.md](docs/AWDL_FEASIBILITY.md)，探针源码在 `tools/`（`nlprobe` / `rawtx` / `delif`）。

## 免责声明

- 与 Google、Samsung 无关，非官方项目。
- 本仓库**不包含**任何 Google 版权 APK；payload 需自行从 Pixel 系统镜像提取。
- 仅用于个人设备研究。刷机/改系统分区有风险，自负其责。

## 已验证环境

| 项目 | 值 |
| --- | --- |
| 机型 | Samsung Galaxy S23 Ultra (SM-S9180, dm3q)，CSC TGY |
| 系统 | Android 17 / SDK 37，One UI 9（`ro.build.version.sep=180000`） |
| Quick Share | ShareLive 13.9.02.31，`protocol_x_supported=true` |
| GMS | 26.45.31（versionCode 264531035） |
| Root | KernelSU 3.3.1（持久或临时均验证过注入逻辑） |

同代高通/猎户座三星、One UI 9 且 `sepolicy` 允许 system_ext overlay 的 KernelSU 设备理论上通用。

## 目录结构

```
module.prop                    # KernelSU 模块描述
customize.sh                   # 安装时校验 payload
service.sh                     # 开机后给 Mosey 预授权限
uninstall.sh
system_ext/priv-app/MoseyApp/MoseyApp.apk                                  # 需自行准备（被 .gitignore 忽略）
system_ext/etc/permissions/privapp-permissions-com.google.android.mosey.xml
tools/prepare-payload.sh       # 把 MoseyApp.apk 放进模块
tools/build.sh                 # 打包 zip
tools/inject-live.sh           # 临时 root：现场注入 + 软重启 + 验证
tools/qsa-inject.sh            # 设备端注入脚本
docs/REVERSE_ENGINEERING.md    # 逆向笔记
```

## 安装 A：持久 root（推荐，开机自动生效）

```sh
# 1. 准备 payload（从 Pixel 系统镜像提取 /system_ext/priv-app/MoseyApp/MoseyApp.apk）
./tools/prepare-payload.sh /path/to/MoseyApp.apk
./tools/build.sh
# 生成 dist/quickshare_airdrop-v1.0.2.zip

# 2. 安装模块
adb push dist/quickshare_airdrop-v1.0.2.zip /data/local/tmp/
adb shell su -c "ksud module install /data/local/tmp/quickshare_airdrop-v1.0.2.zip"
adb reboot
```

卸载：KernelSU Manager 删除模块，或 `su -c "ksud module uninstall quickshare_airdrop"` 后重启。

模块带 privapp 白名单 XML，开机**不需要**改 `ro.control_privapp_permissions`。

## 安装 B：临时 root（不写系统分区，软重启即可）

临时 root 时 KernelSU 不会在开机阶段挂载模块，所以用 bind mount 现场注入：

```sh
SERIAL=<你的序列号> ./tools/inject-live.sh          # 注入 + 软重启 + 验证 + 授权
SERIAL=<你的序列号> ./tools/inject-live.sh --undo   # 撤销
```

脚本流程：推 APK → 建 `/data/adb/qsa_slot`（只放 Mosey）→ `nsenter -t 1 -m` 里 bind mount 覆盖
`/system_ext/priv-app/GoogleFeedback` → `resetprop ro.control_privapp_permissions disable`
→ `ksud soft-reboot` → 轮询 `pm path` → `pm grant`。

> 真重启后临时 root 和注入都会丢失；重新 root 后再跑一次脚本即可。软重启不丢。

## 获取 payload

合法来源只有两类：

1. **Pixel 9 / Pixel 10 系统镜像**（推荐）：解包 `system_ext` 镜像，取
   `/system_ext/priv-app/MoseyApp/MoseyApp.apk`（约 26,903,018 B，v1 jar 签名）。
   参考 sha256：`15224b335b0e4cbb12ce2487cf78cef7a85177465e075fc7eb4f87a67f78bb9a`
   （版本 1.0.922413470 / versionCode 20065，随 OTA 会变）。
2. **APKMirror**：搜 "Quick Share extension"，下载 base APK；**不要**用拆分包（需要合并）。

APK 必须是 Google 签名（`82f9…6878` 或 `b3dc…9115`），否则 ShareLive 会拒绝。

## 验证

```sh
S=R5CW41PT04M
adb -s $S shell su -c "pm path com.google.android.mosey"
# package:/system_ext/priv-app/GoogleFeedback/MoseyApp.apk  （注入模式；模块模式为 .../MoseyApp/MoseyApp.apk）

adb -s $S shell su -c "logcat -d | grep -iE 'CapabilitySource|ProtocolXRepository' | tail"
# [CapabilitySource] isTrustedMoseyInstall= true, FLAG_SYSTEM=true ...
# [CapabilitySource] nearbyShareSupported=true
# [ProtocolXRepository] isProtocolXAvailable: true
# [ProtocolXRepository] syncProtocolXEnabled: providersEnabled -> setProvidersEnabled(true)
```

打开 Quick Share 分享面板，附近 iPhone/iPad/Mac 把 AirDrop 设为「所有人（10 分钟）」后应出现在设备列表。

## 排查

| 现象 | 处理 |
| --- | --- |
| `pm path` 为空 | `adb logcat -d -b all \| grep -iE "mosey\|Failed to parse"`；确认 bind mount 还在 `/proc/1/mounts` |
| `Inconsistent package ... expected com.google.android.mosey` | 一个系统 app 目录只能有一个包；影子目录里只放 Mosey |
| `isTrustedMoseyInstall=false` | APK 不是系统应用（缺 `FLAG_SYSTEM`）或签名不对 |
| `policyPao.protocolXSupported=false` | 三星 PAO 服务端策略，模块层绕不过，需要另行 hook |
| 开机后 `ro.control_privapp_permissions=enforce` 导致扫不到 | 模块模式已带 XML；临时模式脚本会临时改成 disable |

## 许可

脚本与文档：GPL-3.0。Google 的 APK 等第三方内容不在许可范围内，也不随仓库分发。

---

### English (short)

A KernelSU module (plus a temporary-root injector) that installs Google's **Quick Share extension**
(`com.google.android.mosey`) as a privileged system app, which flips Samsung Quick Share's hidden
**Protocol X** flag and enables **AirDrop** interoperability — a feature Google ships only on Pixel 9/10.

The APK is **not** included (Google copyright); extract `MoseyApp.apk` from a Pixel system image and run
`tools/prepare-payload.sh` then `tools/build.sh`. Reverse-engineering notes: [docs/REVERSE_ENGINEERING.md](docs/REVERSE_ENGINEERING.md).

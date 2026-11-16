# 逆向笔记：三星 Quick Share 的 Protocol X（AirDrop）

> 目标设备：SM-S9180（S23 Ultra），Android 17 / SDK 37，One UI 9，ShareLive 13.9.02.31，GMS 26.45.31。
> 工具：jadx 1.5.6（`--show-bad-code`）、apktool/aapt 未用上、logcat、Pixel 9 参考机。
> 结论：功能代码在三星侧是完整的，Google 侧只缺 `com.google.android.mosey` 这个系统应用。

## 1. 名字对照

| 出现位置 | 名称 |
| --- | --- |
| 三星 ShareLive 资源/类 | **Protocol X**（`FEATURE_PROTOCOL_X`、`protocol_x_supported`、`PROTOCOL_X_ONBOARDING`） |
| Google 应用 | `com.google.android.mosey`，APKMirror 名称 **Quick Share extension** |
| 网络接口 | `mosey0`（Mosey 建出来的 AWDL 虚拟网卡） |
| GMS 服务 action | `com.google.android.nearby.SHARING_PROVIDER`（另有 `...LITE_SHARING_PROVIDER`） |

## 2. ShareLive 侧判定链

### 2.1 特性开关（本地第一道门）

`ie/c.java`（混淆后的 `SupportFeature` 枚举）：

```java
FEATURE_PROTOCOL_X = new c("FEATURE_PROTOCOL_X", 22, yc.b.a(170500));
```

`yc/b.java`：

```java
public static boolean a(int i) { return Build.VERSION.SEM_PLATFORM_INT >= i; }
```

`android.os.Build$VERSION`（framework 反编译）：

```java
SEM_INT          = SystemProperties.getInt("ro.build.version.sem", 0);
SEM_PLATFORM_INT = SystemProperties.getInt("ro.build.version.sep", 0);
```

本机 `ro.build.version.sep=180000` ≥ 170500，所以**第一道门本来就开着**（不需要伪造机型）。

### 2.2 能力判定（真正的锁）

`wf/f.java` → `CapabilitySource`：

```java
c()  // GMS getLongVersionCode()
d()  // hasValidMoseySignature：读 com.google.android.mosey 的 signingInfo，
     // SHA-256 必须在 {82f90326…6878, b3dc8d36…9115} 里
i()  // isTrustedMoseyInstall：ApplicationInfo.flags 必须含
     // FLAG_SYSTEM(1) 或 FLAG_UPDATED_SYSTEM_APP(128)  ← 普通侧载在这里死
f()  // isMoseySupported = g() && d() && i()
     //   && queryIntentServices(new Intent("com.google.android.nearby.SHARING_PROVIDER")
     //        .setPackage("com.google.android.mosey"), 0) 非空
     //   && c() >= 261125000
g()  // GMS 已启用 && c() >= 235000000 && !isSecureFolderMode()
```

调用方 `bg/m7.java`（`supportsProtocolX`）：

```java
if (!ie.c.F.f15687p || !prefs.getBoolean("protocol_x_supported", true)) return false;
// 否则组合 policyPao + providers + isMoseySupported
```

`b6/a.java` 打印：

```
[ProtocolXRepository] supportsProtocolX: isMoseySupported=…, isProtocolXSupported=…, supported=…
```

能力就绪后 `bg/p7.java` 会 `setProvidersEnabled(true)`，`bg/n7.java` 打印 `isProtocolXAvailable: true`。

### 2.3 权限交接

`pd/b.java` 读取 Mosey 的 `requestedPermissions`，把没授的 dangerous 权限挑出来，发
`com.google.android.gms.nearby.sharing.action.REQUEST_PERMISSIONS` 给
`com.google.android.mosey` 的 `MoseyPermissionActivity`。所以必须提前 `pm grant`，否则用户点分享时会被弹窗打断。

### 2.4 传输态判断

`pm/b1.java` 遍历 `NetworkInterface.getNetworkInterfaces()` 找 `mosey0`，用来判断
`isProtocolXInUse`（AirDrop 传输中要暂停 Wi-Fi 直连等）。Mosey 另外注册了
`com.google.android.mosey.fileprovider`（`bg/l1.java` 里做 uri authority 校验）。

## 3. Mosey 的硬性要求

| 要求 | 说明 |
| --- | --- |
| Google 签名 | SHA-256 `82f90326951d14e08010b2a26cce329be2b09e74752158b7640e04a544676878`（Pixel 系统镜像版）或 `b3dc…9115` |
| 系统应用 | `ApplicationInfo.flags & (FLAG_SYSTEM \| FLAG_UPDATED_SYSTEM_APP)` 非 0 |
| 服务 | `com.google.android.nearby.SHARING_PROVIDER`，权限 `com.google.android.gms.permission.ACCESS_NEARBY_SHARE_API` |
| GMS 版本 | ≥ 26.11.25（`wf/b0.java` 里 `MOSEY(261125000)`；`MOSEY_PROVIDERS_ENABLED(261825000)`、`T2S_WITH_ANDROID(263125000)` 是后续门槛） |

Pixel 上 Mosey 位于 `/system_ext/priv-app/MoseyApp/MoseyApp.apk`，
privapp 白名单在 `/system_ext/etc/permissions/privapp-permissions-com.google.android.mosey.xml`（8 个特权权限）。

Mosey 自己实现了 AWDL/Apple 协议：其 `classes2.dex` 里有 `apple/Apple/APPLE`、`mosey0`、`_airdrop._tcp`、`AirDrop` 等字符串。

## 4. 踩坑：PackageManager 一个目录只能有一个包

把 Mosey 塞进现有 `/system_ext/priv-app/GoogleFeedback/` 做影子目录时，如果同时保留原 APK：

```
W PackageManager: Failed to parse /system_ext/priv-app/GoogleFeedback:
  Inconsistent package com.google.android.feedback in
  /system_ext/priv-app/GoogleFeedback/GoogleFeedback.apk; expected com.google.android.mosey
```

整个目录扫描作废（原 App 消失、Mosey 也装不上）。正确做法：目录里**只留一个包**，
让被覆盖的目录里的原 APK 被遮蔽；真重启后挂载消失，原 App 自动回来。

另一个坑：不要用 0 字节占位文件做嵌套 bind mount——父目录挂载会遮蔽子文件挂载，
PM 扫到坏 APK 同样中断。

## 5. 特权权限校验

三星默认 `ro.control_privapp_permissions=enforce`。正规做法是随模块提供
`system_ext/etc/permissions/privapp-permissions-com.google.android.mosey.xml`（本模块已带）。
临时 root 的注入模式没法往 `/system_ext/etc/permissions` 里加文件，所以在注入后用
`resetprop ro.control_privapp_permissions disable` 临时放宽（soft reboot 后仍有效，真重启复原）。

## 6. KernelSU system_ext overlay

KernelSU 3.3.1 的 `ksud` 里有 `handle_partition system_ext`，模块根目录的 `system_ext/`
会在开机阶段 overlay 到 `/system_ext`，因此**持久 root** 用模块 zip 即可，无需 bind mount。
临时 root（开机时 KernelSU 不启动）才需要现场注入。

## 7. 临时 root 注入流程

```
/data/adb/qsa_slot/MoseyApp.apk                 # 只放 Mosey
nsenter -t 1 -m -- mount --bind /data/adb/qsa_slot /system_ext/priv-app/GoogleFeedback
resetprop ro.control_privapp_permissions disable
ksud soft-reboot                                 # 只重启框架，不重启内核，不掉临时 root
```

软重启后 system_server 重新扫描 `/system_ext`，Mosey 以 `FLAG_SYSTEM | PRIVILEGED | SYSTEM_EXT` 安装。

## 8. 实测日志（SM-S9180）

```
I ShareLive:QuickShare: [CapabilitySource] isTrustedMoseyInstall= true, FLAG_SYSTEM=true, FLAG_UPDATED_SYSTEM_APP=false
I ShareLive:QuickShare: [CapabilitySource] nearbyShareSupported=true
I ShareLive:QuickShare: [CapabilitySource] protocol = 31
I ShareLive:QuickShare: [ProtocolXRepository] isProtocolXAvailable: true
I ShareLive:QuickShare: [ProtocolXRepository] syncProtocolXEnabled: providersEnabled -> setProvidersEnabled(true)
I ShareLive:QuickShare: [NearbyShareSourceImpl] NS [ setProvidersEnabled ] success : enabled=true
I ShareLive:QuickShare: [ExternalServiceRepository] isMoseyPermissionMissing=false
I ShareLive: [SLog][main][DeviceSettingViewModel] viewState: ... protocolXSupport=true ...
I ShareLive: [SLog][main][MainActivityViewModel] viewEffect: LaunchOnboardingTipsDialog(protocolXSupported=true)
```

`MoseyPermissionActivity` 的权限弹窗没有出现，说明 23 个运行时权限的预授权是有效的。

## 9. 未验证 / 待办

- 真机对传（需要一台 iPhone）尚未验证；只验证到“发现通道已注册、provider 已启用”。
- 三星 PAO 服务端策略若有 `policyPao.protocolXSupported=false`，本地无法绕过。
- 不同机型 `system_ext` 的 SELinux 标签/挂载参数可能不同，需要按机型确认 overlay 是否生效。

## 10. 固件依赖：Mosey 系统服务（关键限制）

在 SM-S9180 上把链路打通后（ShareLive `isProtocolXAvailable=true`，GMS 日志出现
`A new client has bound ... for calling package com.google.android.mosey` 与
`External sharing provider has started discovery`），Mosey 进程初始化仍然失败：

```
I/Mosey.A: API: start() called with options: DiscoveryOptions(mode=1, ...)
I/Mosey.A: Wakeup advertising started with settings: ...
E/ServiceManager: error in isDeclared
E/ServiceManager: java.lang.SecurityException: SELinux denied for service.
I/Mosey.A: Mosey Lib is not available.
E/Mosey.A: java.lang.IllegalStateException: Mosey service is not available.
I/Mosey.A: Event received: InitializationFailed(cause=... Mosey service is not available.)
```

设备上确认：

```
$ su -c "service check Mosey"
Service Mosey: not found
$ service list | grep -iE "mosey|awdl"   # 无结果
```

结论：Mosey 只是一个客户端，它通过 binder 调用系统里的 `Mosey` 服务（Google 在 Pixel 固件中提供的 AWDL/射频实现）。
One UI 既没有该 native 服务，也没有对应 `service_contexts`，因此模块/普通 App 无法补出；这属于固件能力差异，
与 ShareLive 的开关无关。本模块能做到的边界就是「ShareLive 认账、扩展被 GMS 绑定并启动 discovery」。

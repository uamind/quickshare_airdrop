# AWDL 与 S23 Ultra 的可行性实测

这份文档回答一个问题：**在不依赖 Pixel 固件的前提下，能不能自己实现 AirDrop 那层（AWDL）？**

结论先说：**在 SM-S9180（One UI 9 / Android 17）上不行**。不是 App 层的开关问题，也不是 SELinux 问题，
而是 `cfg80211` 这一层就不允许 userspace 控制「monitor 接口的信道」，而 AWDL 的本质就是**在保持普通 Wi-Fi 的同时
按时间片在 6 / 44 / 149 三个 social channel 上高速跳频收发**。没有信道控制权，就没有 AWDL 的时间同步窗口，
也就没有 AirDrop。

下面是把结论一步步测出来的过程，命令与输出都可以复现。

---

## 1. AirDrop 对射频的要求

AWDL（Apple Wireless Direct Link）不是普通的点对点 Wi-Fi，它要求：

| 要求 | 说明 |
| --- | --- |
| 原始 802.11 帧收发 | 收发带 Apple OUI `00:25:00` / type `0x12` 的 vendor-specific **action frame**，不能用 socket 语义的 Wi-Fi 完成 |
| 信道调度 | 在 social channel **6 / 44 / 149** 之间按同步参数（`SyncParams`：availability window 序列、周期、对齐）跳频，每秒几百到上千次 |
| 与普通 Wi-Fi 共存 | 跳频期间不能断开用户当前的 Wi-Fi 连接（这是最难的一条） |
| 主节点选举 / 时钟同步 | 用 action frame 里的 AWDL 信标完成 |

AirDrop 本身跑在 AWDL 之上：先 mDNS/Bonjour 发现 `_airdrop._tcp`，再连对端 `TCP 8770` 的 "Ask" HTTP 服务。

## 2. Pixel 上 Google 是怎么做的

Pixel 固件里，这件事由一组**系统级组件**完成，而不是 App 层：

```
/system_ext/bin/mosey_server                    # 原生守护进程, user system, cap NET_ADMIN NET_RAW
/system_ext/lib64/libmosey_daemon_ffi.so        # nl80211 / 射频实现 (Rust)
/system_ext/etc/init/mosey.rc                   # 由 init 拉起
```

运行期可以看到它建立的东西（Pixel 9 实测）：

```
46: aware_nmi0:  link/ether ...              # Wi-Fi Aware (NAN) 接口
47: wlan0:       link/ether ...  state UP    # 普通 Wi-Fi 连接
48: wlan1:       link/ether ...
49: wondertap0:  link/ieee802.11/radiotap    # 原始 802.11 收发口 (radiotap = monitor 语义)
```

以及一个 binder 服务：

```
$ service list | grep -i mosey
com.google.android.moseyservice.IMoseyService/default
```

Mosey App（`com.google.android.mosey`）通过这个 AIDL 服务拿到射频能力，ShareLive / GMS 再通过
`SHARING_PROVIDER` 与之对接。**整个链路的关键在 `mosey_server` + 内核/固件那一层，App 只是调用方。**

所以在没有该服务的机型上，模块最多只能把 ShareLive 的开关和 Mosey 装好，射频那一层仍然缺失——这也是本仓库
README 里「固件限制」一节的由来。

## 3. 移植 `mosey_server` 到 S23 Ultra 的实测

在 SM-S9180（KernelSU 临时 root）上做过完整移植尝试：

1. 把 `mosey_server` 与 `libmosey_daemon_ffi.so` 放到 `/data/adb/mosey_x/`；
2. 二进制里硬编码的 `dlopen` 路径是
   `vendor/google/services/QuickShareExtension/src/lib.rs:33:62`
   指向 `/system_ext/lib64/libmosey_daemon_ffi.so`，做等长改写后可以正常加载；
3. 卡在服务注册：

```
Failed to register service: UNKNOWN_ERROR   (server.rs:33:6)
```

原因是 servicemanager 的 `service_contexts` 里没有
`com.google.android.moseyservice.IMoseyService` 的映射，而这张表**只在 servicemanager 启动时读取**：

```
$ su -c "service check Mosey"
Service Mosey: not found
```

想补映射必须重启 servicemanager；`servicemanager` 是 critical 服务，`setprop ctl.restart servicemanager`
会导致 system_server 一起重启（实测把机器重启了、并且掉了临时 root）。也就是说这条路线在「临时 root + 不想重启」
的前提下是死路；即使能重启补上映射，也还要面对下面的射频问题。

## 4. S23 Ultra 的射频能力实测（本轮关键结论）

为了排除「三星硬件到底行不行」这个疑问，写了三个不依赖 libnl 的小工具直接跟内核 `nl80211` 对话
（源码在本仓库 `tools/`，用 `zig cc -target aarch64-linux-musl -static` 交叉编译）。
这样就不依赖 Pixel 固件、也不依赖任何 App，纯粹问内核「你能干什么」。

### 4.1 现有接口与 wiphy 能力

```
$ su -c "/data/local/tmp/nlprobe"
nl80211 family id=38
wiphy 0 phy0     iftypes: <none reported>
iface idx=44  wiphy=0 name=wlan0         type=STATION
iface idx=45  wiphy=0 name=p2p0          type=P2P_DEVICE
iface idx=46  wiphy=0 name=wifi-aware0   type=NAN
```

- `wifi-aware0` 存在 ⇒ 这块高通 Wi-Fi **支持 Wi-Fi Aware (NAN)**，也就是「时间片 + 可用窗口」这套硬件机制是有的；
- 但驱动没有上报 `NL80211_ATTR_SUPPORTED_IFTYPES`，所以「支持哪些接口类型」只能靠实际创建来验证。

### 4.2 能不能建 monitor（radiotap）接口：能

```
$ su -c "/data/local/tmp/nlprobe create"
NEW_INTERFACE(type=monitor) -> ...

$ ip -o link show | grep qsamon0
47: qsamon0: <BROADCAST,MULTICAST> ... link/ieee802.11/radiotap be:10:7b:e8:3d:fa ...
```

`qsamon0` 真的建出来了，`link/ieee802.11/radiotap` 跟 Pixel 的 `wondertap0` 属于同一类接口。
**也就是说「原始 802.11 收发口」这一层，S23 Ultra 的驱动是给得出来的。**（这一步已经翻转了「S23 硬件根本不行」的直觉。）

### 4.3 能不能控制这个接口的信道：不能（决定性的一条）

AWDL 必须让这个接口在 6 / 44 / 149 之间跳。于是对 `qsamon0` 发 `NL80211_CMD_SET_WIPHY` + `WIPHY_FREQ=2437`：

```
$ su -c "/data/local/tmp/rawtx qsamon0 2437 3"
SET_WIPHY freq=2437 -> Resource busy (-16)
send #0 ok (61 bytes)
send #1 ok (61 bytes)
send #2 ok (61 bytes)
```

`-EBUSY` 来自 `cfg80211`：

```c
int cfg80211_set_monitor_channel(...)
{
    if (!cfg80211_has_monitors_only(rdev))
        return -EBUSY;          /* 只有「phy 上全是 monitor 接口」时才允许设信道 */
    return rdev_set_monitor_channel(rdev, chandef);
}
```

而 Android 上 `wlan0` 永远存在（系统 Wi-Fi 主接口，且 `num_ifaces` 统计的是所有已创建的接口，把 Wi-Fi 关掉也不算
"monitors only"）。实测把 Wi-Fi 关掉再试，仍然 `-EBUSY`：

```
$ svc wifi disable
$ su -c "/data/local/tmp/rawtx qsamon0 2437 2"
SET_WIPHY freq=2437 -> Resource busy (-16)
```

**结论：在这套内核上，userspace 无法让 monitor 接口按 AWDL 的信道序列跳频。**
`sendto()` 虽然返回成功（驱动收下了这个 skb），但射频停在哪条信道是不受控的——发出去的帧对端收不到，
收也收不到对端的 AWDL 信标，同步窗口就无从谈起。

补充一条会让复现者踩坑的细节：cfg80211 的 `DEL_INTERFACE` **只认 `NL80211_ATTR_IFINDEX`**，用接口名会返回
`EINVAL`，而 `ip link del` 会返回 `Operation not supported on transport endpoint`：

```
$ su -c "ip link del qsamon0"
RTNETLINK answers: Operation not supported on transport endpoint
$ su -c "/data/local/tmp/delif qsamon0"     # 用 ifindex
DEL_INTERFACE qsamon0 -> OK
```

### 4.4 那 Pixel 凭什么行？

Pixel 上 `wondertap0` 与 `wlan0` 是同时存在的，按 4.3 的 `cfg80211` 规则，Google 也**不可能**走
`cfg80211_set_monitor_channel` 这条路。差别在于 Pixel 的 Wi-Fi 驱动/固件给自家 `mosey_server` 留了
**vendor 私有通道**（连同 NAN 的可用窗口调度一起），这是 SoC 厂商 + 固件级的能力，不是 SELinux 策略、
也不是 Magisk/KernelSU 模块能补出来的东西。

这也解释了三星官方口径为什么只列 **S24 / S25 / S26、Z Fold 6 / Z Flip 7 / Tri-Fold**，而把 S23 排除在外：
差分不在 App，在 Wi-Fi 驱动/固件有没有为这套「边连 Wi-Fi 边跳频收发原始帧」开通道。

## 5. 最终判断

| 层次 | S23 Ultra 现状 | 能否靠 root/模块补 |
| --- | --- | --- |
| ShareLive 的 Protocol X 开关 | 已打开（本模块 v1.0.2） | ✅ 已实现 |
| Mosey App（`com.google.android.mosey`）作为系统应用 | 已装进 `/system_ext`，签名校验通过 | ✅ 已实现 |
| GMS 绑定外部 `SHARING_PROVIDER` | 已可用（需手动拉起 `ExternalSharingService`） | ✅ 已实现 |
| `Mosey` 系统服务（AIDL + SELinux service_contexts） | 缺失；补映射需 servicemanager 重启 | ⚠️ 需重启，临时 root 下不可行 |
| NAN 时间片能力 | 硬件/驱动有（`wifi-aware0`） | — |
| monitor/radiotap 原始帧接口 | 驱动允许创建 | — |
| **monitor 接口信道控制（AWDL 跳频）** | **`cfg80211` 拒绝（`-EBUSY`）** | ❌ 需要驱动/固件支持，root 无法补 |

**所以：** 想让 S23 Ultra 直接对 Mac 做 AirDrop，靠模块/root 这条路是走不通的，瓶颈在 Wi-Fi 驱动与固件，
与 Pixel 的差距是「厂商私有射频通道」。本模块的价值是把 ShareLive/GMS 侧该开的都开好——一旦换到 S24 及之后
（或任何自带 `mosey_server` 的固件），这套开关就能直接生效。

### 5.1 「那把固件刷进去」也不行

有人会想到「既然要驱动/固件支持，那我刷个改过的固件」。这条路在 S23 Ultra 上是双重堵死的：

**第一重：BL 锁着。** 测试机 `ro.boot.verifiedbootstate=green`，说明 bootloader 未解锁、AVB 校验通过；
root 是靠临时提权拿到的，不修改 boot 分区，所以一重启就要重新获取。要改内核/驱动就得往 `boot` / `vendor` / `vendor_dlkm`
分区写东西，锁 BL 的状态下这些都刷不进去。

**第二重：就算解锁了也办不到。** 真正的卡点在 **Wi-Fi firmware blob**——它随 SoC 以二进制形式发布，闭源、签名，
不属于三星开源的 GPL 内核代码。改内核最多只能拆掉 `cfg80211` 那道闸（4.3 里的 `monitors-only` 检查），而闸后的
`set_monitor_channel()` 由 qcacld 驱动实现，真正执行「按时间片切信道、在窗口里收发原始帧」的是固件。
固件里没有这条私有通道，就是没有——我们既无法生成，也无法给闭源固件打补丁。

解锁 BL 的代价（仅供参考，本仓库不推荐）：抹掉全部数据、KNOX 熔断（`0x1`）导致 Samsung Pay / Pass / 安全文件夹
永久失效、部分银行与企业 App 拒绝运行、OTA 只能手动刷。换来的只是「有可能改内核」，而不是「能用 AirDrop」。

**结论：** S23 Ultra 想走 AirDrop，只有换机（S24 / S25 / S26、Z Fold 6 / Z Flip 7 / Tri-Fold）或等厂商在固件里
补齐这条通道两条路。模块本身不需要改，它已经把 App/系统层该做的做完了。

## 6. 复现方法

```bash
# 交叉编译（macOS + zig，无需 NDK）
cd tools
zig cc -target aarch64-linux-musl -Os -static nlprobe.c -o nlprobe
zig cc -target aarch64-linux-musl -Os -static rawtx.c   -o rawtx
zig cc -target aarch64-linux-musl -Os -static delif.c   -o delif

adb push nlprobe rawtx delif /data/local/tmp/
adb shell 'su -c "chmod 755 /data/local/tmp/{nlprobe,rawtx,delif}"'

adb shell 'su -c "/data/local/tmp/nlprobe"'                  # 接口与 wiphy 能力
adb shell 'su -c "/data/local/tmp/nlprobe create"'           # 尝试建 monitor 接口
adb shell 'su -c "/data/local/tmp/rawtx qsamon0 2437 3"'      # 设信道 + 注入 AWDL action frame
adb shell 'su -c "/data/local/tmp/delif qsamon0"'             # 清理
```

> 提示：`sendto()` 返回成功**不代表帧真的发到了空中**。判断射频是否可控，要看 `SET_WIPHY` 的返回码，
> 以及能否在第三方设备上用 monitor 抓到自己注入的帧。

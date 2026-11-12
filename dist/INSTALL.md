# AMD xHCI 开机卡住修复 — OpenCore 安装包

这个文件夹是给你**本机 AMD 平台**用的，和局域网那台 Intel Mac 无关。里面只有一个 kext：

```
Kexts/AMDXHCIPreflight.kext      # 26.7.3 上真实编译出来的 x86_64 kext
```

它做一件事：在 `AppleUSBXHCIPCI` 绑定 XHCI 控制器**之前**，把控制器恢复到确定状态（BIOS/OS 交接、halt、HCRST、清 CRCR/DCBAAP/CONFIG、W1C 清端口状态、关 MSI/MSI-X），然后主动退出、把控制器让给 Apple 驱动。它不接管 USB、不建隧道、不改 ACPI，也不需要禁用任何 XHCI 控制器。

---

## 1. 放到 EFI 里

```
EFI/OC/Kexts/AMDXHCIPreflight.kext
```

## 2. config.plist → Kernel → Add

把下面这一条**加在 `Kernel → Add` 数组里**（顺序有影响：它要靠 `IOProbeScore 10000` 抢在 Apple 驱动前面，放在同类的 USB kext 之前更稳）：

```xml
<dict>
    <key>Arch</key>              <string>x86_64</string>
    <key>BundlePath</key>        <string>AMDXHCIPreflight.kext</string>
    <key>Comment</key>           <string>AMD xHCI preflight (no SSDT needed)</string>
    <key>Enabled</key>           <true/>
    <key>ExecutablePath</key>    <string>Contents/MacOS/AMDXHCIPreflight</string>
    <key>MaxKernel</key>         <string></string>
    <key>MinKernel</key>         <string>19.0.0</string>
    <key>PlistPath</key>         <string>Contents/Info.plist</string>
</dict>
```

`MinKernel 19.0.0` = **Catalina 10.15 起**（19.x）。这个 bug 是 AMD 平台特有的、与 macOS 版本无关（Catalina 上装不装原版 fork 都卡），所以兼容范围直接从 Catalina 划起。kext 只依赖 `com.apple.kpi.*` 和 `com.apple.iokit.IOPCIFamily`，plist 里声明的是 `IOPCIFamily 2.6`（不是编译机上的 2.9），同一个二进制从 Catalina 一直能加载到 Tahoe：

| 系统 | `MinKernel` |
|---|---|
| Catalina 10.15 | `19.0.0` |
| Big Sur 11 | `20.0.0` |
| Monterey 12 | `21.0.0` |
| Ventura 13 | `22.0.0` |
| Sonoma 14 | `23.0.0` |
| Sequoia 15 | `24.0.0` |
| Tahoe 26 | `25.0.0` |

Mojave 10.14 / High Sierra 10.13 没验过（要试就把 `MinKernel` 改成 `18.0.0` / `17.0.0`）。`MaxKernel` 留空表示不设上限。

## 3. 别同时装旧的 fork

`GUX-RyzenXHCIFix` / `GenericUSBXHCI` 必须从 `Kernel → Add` 里删掉或 `Enabled=false`。两者都去碰同一个控制器，叠加只会更难排查。

## 4. 开机验证

```bash
# kext 有没有起来
kmutil showloaded --list-only | grep -i amdxhci

# 入口跑没跑（这行是 kext 自己的 IOLog）
log show --last boot --predicate 'eventMessage CONTAINS "AMDXHCIPreflight"' | tail -5
# 期望: AMDXHCIPreflight: entry ok, 1 static ctor(s) run

# preflight 到底做了什么：控制器上会多出这些属性
ioreg -rc IOPCIDevice -l | grep -E 'amdxhci-(preflight|phantom|dryrun|status)'
# 期望: "amdxhci-preflight" = "ok 0x1022:0x43ee cap=0x40 ver=0x0110 ports=8 slots=64 legs=0x110 ho=ok rst=ok/20ms rx=0x... msi=3 t=20 flags=0x3d80"
# 另有 amdxhci-flags / -status / -ports / -legacy-cap / -reset-attempts / -total-ms / -bar-bytes / -hci-version
```

### 4.1 先干跑一次（不动硬件，先证明链路通）

第一次上车建议先加两个 boot-arg：

```
-amdxhci_anyvendor -amdxhci_dryrun
```

`-amdxhci_dryrun` 让 preflight 只做**分类和报告**：一个 MMIO 写都没有、一个 PCI 配置写都没有（核心库在 `Run()` 里强制把所有写路径关掉，不是靠调用方自觉），报告里会带 `dry` 标志和 `"amdxhci-dryrun" = true`。`-amdxhci_anyvendor` 让它在非 `0x1022` 的控制器上也能匹配，这样**任何一台机器**都能用来验证「入口点 → 类注册 → personality → start → 报告」这条链，而不只是 AMD 机。

看到 `entry ok` + `amdxhci-preflight` 一行 + `amdxhci-dryrun = true`，就说明 kext 这一侧完全正常；之后去掉这两个参数重启，才是真正动硬件的那一次。这一步在 AMD 机上同样安全，可以先跑。

三个都要看到，缺哪个决定了问题在哪：

| 现象 | 含义 |
|---|---|
| 没有 `entry ok` | kext 没被注入/没加载 → 检查 `Kernel → Add` 与 `Enabled` |
| 有 `entry ok` 但没有 `amdxhci-preflight` 属性 | personality 没匹配上 → 看 `ioreg -p IODeviceTree -n` 里控制器的 `vendor-id` 是不是 `0x1022`；或 `IOProbeScore` 被别的驱动压过 |
| 有属性但 `status=bailed` / `flags` 带 `0x1` | BAR0 没映射好（`kFlagPhantom`）→ 是 ACPI/PCI 资源问题，不是本 kext 的问题 |
| 有属性且 `rst=ok`，但开机还是偶尔卡 | 把 `flags` 和 `t=` 发我，按下面开关逐项二分 |

## 5. boot-args 开关（二分/回滚用）

| 参数 | 作用 |
|---|---|
| `-amdxhci_off` | 完全停用（等价于不装） |
| `-amdxhci_nohandoff` | 不做 BIOS→OS 交接 |
| `-amdxhci_noreset` | 不做 halt + HCRST |
| `-amdxhci_noports` | 不动端口状态位 |
| `-amdxhci_nomsi` | 不关 MSI/MSI-X |
| `-amdxhci_rxdetect_all` | 所有端口强制回 RxDetect |
| `-amdxhci_dryrun` | 只分类、只出报告，**零写入**（见 4.1） |
| `-amdxhci_anyvendor` | 允许在非 AMD 控制器上匹配（只配合 dryrun 用） |
| `-amdxhci_flr` | 先做 PCIe Function Level Reset（整个 function 打回热复位状态） |
| `-amdxhci_noflr` | 明确不做 FLR（默认不做） |
| `amdxhci_flr_ms=N` | 等 FLR 完成的超时，默认 200 |
| `amdxhci_cnr_ms=2000` | 等 CNR 的超时（默认 1200） |
| `amdxhci_reset_ms=1500` | 等 HCRST 的超时（默认 1000） |
| `amdxhci_handoff_ms=2000` | 等 BIOS 放手的超时（默认 1000） |
| `amdxhci_retries=3` | HCRST 重试次数（默认 2） |
| `amdxhci_log=2` | 日志更啰嗦 |

## 6. 回滚

把 `Kernel → Add` 里那条的 `Enabled` 改成 `false` 即可，别的都不用动；临时验证也可以加 `-amdxhci_off`。

## 6.1 如果默认参数仍然偶尔卡

先看 `amdxhci-preflight` 那一行的 `rst=` 和 `flags=`：

- `rst=fail` 或 `flags` 带 `0x200`（`kFlagResetTimeout`）→ HCRST 没完成，说明控制器比预期更死。加 `-amdxhci_flr`：它在这个 function 上先做 PCIe Function Level Reset，把 DMA 绑定、MSI 表、端口供电一起打回热复位状态，再走后面的 halt/reset。报告会多一段 `flr=ok`（属性 `amdxhci-flr`）。
- `ho=fail` 或 `ho=forced` → 固件一直不放 LEGSUP 信号量。`forced` 表示我们已经强制清掉了，通常够用；连续出现可以试 `-amdxhci_nohandoff` 反向验证是不是这一步本身在拖时间。
- `rx=` 只有少量位、但端口就是起不来 → 试 `-amdxhci_rxdetect_all`。
- 全部 `ok` 但开机仍偶发卡 → 问题可能不在我们这一步之前，而在 Apple 驱动自己的端口枚举；把整行发我，我按 `t=`、`ports=`、`legs=` 判断。

`-amdxhci_flr` 默认关闭是有意的：少数固件声称支持 FLR、实际按下就再也回不来，没有同型号机器可测时不该默认按这个按钮。

---

## 附：为什么是这套东西，而不是继续用 GUX

26.7.3 上实测（`nm` 直接查 `BootKernelExtensions.kc`）：

- `IOUSBControllerV3` 相关符号 **0 个**；
- 加载 fork 时报 `Cannot find symbol for metaclass ... 'IOUSBControllerIsochListElement::gMetaClass'`。

也就是说 GUX 那套「继承 `IOUSBControllerV3` + 10.10 头文件」的实现在 Tahoe 上**根本进不了内核**。它以前能「起作用」只能靠 OpenCore 重建内核集合时一起重链，而那套 vtable 布局和当前系统对不上 —— 这正好解释为什么 Sonoma 之后开始偶发卡死、而 SSDT 少一个控制器就能缓解。

这个 kext 不继承任何 USB 类，只依赖 `com.apple.kpi.iokit` / `com.apple.kpi.libkern` / `com.apple.iokit.IOPCIFamily` 三样长期稳定的东西，所以不会随 USB 栈改版而错位。

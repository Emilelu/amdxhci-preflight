# AMD xHCI 开机卡住修复：把「碰运气」的退出改成确定性的交接

针对 [RattletraPM/GUX-RyzenXHCIFix](https://github.com/RattletraPM/GUX-RyzenXHCIFix) 在 macOS Sonoma / Sequoia / Tahoe 上偶发开机卡住的问题（SSDT 禁用一个 XHCI 控制器就正常）。

---

## TL;DR

两条路径，选一条，**不要同时装**：

| | 做法 | 适用 |
|---|---|---|
| **A** | 把 `gux/GUX-Preflight.patch` 打进 GUX-RyzenXHCIFix，替换掉它原来那套「故意失败退出」的把戏 | 你已经在用这个 fork，想最小改动 |
| **B**（推荐） | 删掉 GUX-RyzenXHCIFix，装独立的 `AMDXHCIPreflight.kext` | 干净、不继承 10.10 的 vtable、可与任何 USB 驱动共存 |

两者共用同一份 preflight 实现（`core/AMDXHCIPreflight.h`），行为完全一致。

---

## 1. 现在这个 fork 到底在干什么

`GenericUSBXHCI/V1Pure.cpp` 的 `IOReturn CLASS::UIMInitialize(IOService*)` 里，上游原本是：

```cpp
if (!_filterInterruptSource) {
    IOLog("%s: Unable to create filterInterruptEventSource\n", __FUNCTION__);
    UIMFinalize();
    return kIOReturnNoResources;
}
```

commit `b7c0850`（"make GUX exit @ filterInterruptEventSource"）只做了一件事——把这两行大括号**注释掉**：

```cpp
//if (!_filterInterruptSource) {
    IOLog(...); UIMFinalize(); return kIOReturnNoResources;
//}
```

于是**无条件**走到这个 return。start 失败 → IOKit 换下一个候选者 → AppleUSBXHCIPCI 接手。README 里写得很坦白：

> GUX would get loaded and perform some of its USB init functions, then ... it would error out and exit during early boot, letting AppleUSBXHCIPCI attach.

### 关键事实：走到这个 return 之前，它对硬件几乎什么都没做

读完整个 `UIMInitialize` 的前半段，退出点之前只有这些：

1. `mapDeviceMemoryWithIndex(0)` —— 映射 BAR0
2. `SetVendorInfo()`、`GetErrataBits()` —— 纯软件
3. `_device->configWrite16(kIOPCIConfigCommand, kIOPCICommandMemorySpace)` —— **写而不是或**
4. 读 `HCSParams[0]` 三个只读字段
5. `findInterruptIndex()` —— 读 PCI 中断类型
6. 建一个 `IOFilterInterruptEventSource` 然后丢掉

而**下面这些才是真正让控制器可用的步骤，一个都没执行**：

- `Read32Reg(HCCParams)` → `DecodeExtendedCapability()`（解析 XECP 链）
- `TakeOwnershipFromBios()`（LEGSUP 交权）
- `DisableComplianceMode()`
- `ResetController()`（HCRST，且会等 CNR）
- `InitializePorts()`（端口从 Polling/Compliance 拉回 RxDetect）
- 写 `Config`/`DNCtrl`、分配 DCBAA / 命令环 / 事件环 / scratchpad

**结论：这个 fork 的「修复」不是修复，只是一个副作用。** 它把控制器从固件手里接过来之前就撒手了，接手的 Apple 驱动看到的是固件留下的任意状态。

### 于是「偶发」从哪来

AppleUSBXHCIPCI 假定自己从一个静止的控制器启动。遗留状态一旦踩中下面任意一条，就会在启动早期长时间自旋（表现为光标卡住、进度条不动、`IOConsoleUsers` 之后黑屏）：

| # | 遗留状态 | 后果 |
|---|---|---|
| 1 | `USBLEGSUP.BIOS_OWNED` 仍置位 | BIOS 的 SMI 继续插手端口状态，HCRST / port reset 被反复打断 |
| 2 | PCI `BusMaster` 被上面第 3 步清掉 | 控制器写出的 DMA 无效，命令环/事件环握手超时 |
| 3 | USBSTS.CNR（Controller Not Ready）未 drain 就 HCRST | HCRST 永不自清，驱动死等 |
| 4 | 端口停在 Polling / Recovery / HotReset / Compliance | port reset 超时（这一步最常表现为「卡在 2 分钟以上然后放弃」） |
| 5 | 固件使能了 MSI/MSI-X 但中断未路由 | 等一个永远不来的中断 |
| 6 | `IOProbeScore = 1` | 与任何 score ≥ 1 的候选者（注入的 kext、Apple 自己更新过 score 的驱动）争抢，谁先 start 不确定 |

第 1、2 条是**固定发生**的（fork 从不交权，且固定清掉 bus-master），第 3–5 条取决于固件这次留下的状态，第 6 条取决于加载顺序。四条叠加，就是「有时能开、有时不能」。

### 为什么 SSDT 禁一个控制器就好了

AMD 平台一般有两个 xHC（CPU 侧 + 芯片组侧），两个 `IOPCIDevice` 会**并行** probe/start。它们共享 PCIe 链路和 ACPI 电源状态，任何一个的 SMI / reset 都能拖住另一个，而 fork 恰好让两者都处于未交权的状态。

禁掉一个 = 少一个竞争者，剩下的那个单独走完 probe→start，撞上竞态的窗口小得多。

> 这一条是从「禁掉就好」反推的因果，属于**推理**，不是抓到的 trace。但它与「两个控制器并行 + 都未交权」的现象一致，也是本修复要彻底消除的东西：把不确定性换成确定性动作后，无论一个还是两个控制器都不再依赖时序。

---

## 2. 修复方案：把退出前的动作补全

`core/AMDXHCIPreflight.h` 是一个 header-only、不依赖 IOKit 的状态归一器。它在 fork 里替换掉那段假退出，在独立 kext 里作为 `start()` 的主体。

执行顺序（每一步都有界，任何一步超时都**只打 flag 不中断**，绝不会把开机挂死）：

| 步 | 动作 | 为什么 |
|---|---|---|
| 0 | `Classify()`：BAR < 4K、cap 区读回全 1/全 0、cap_length / HCI version / ports / slots 越界 → 判为幻影控制器 | **零写入**直接退出。AMD 平台上空的 PCI 槽会被类匹配到，往幻影设备写寄存器就是挂死 |
| 1 | PCI `Command |= MemorySpace\|BusMaster`；PMCSR 低 2 位非 0 则写 0 进 D0 | 固件常把设备留在 D3 或关着 bus-master，此时碰 MMIO 本身就是错的 |
| 2 | 等 `USBSTS.CNR` 清零（默认 1200 ms） | CNR=1 时 HCRST 无效。超时不清也不放弃，记 `kFlagCnrTimeout` 继续 |
| 3 | 走 XECP 链找 capId=1；**按规范偏移**写 `cap+4`（USBLEGSUP）`OS_OWNED`，轮询等 `BIOS_OWNED` 清（默认 1000 ms） | 这才是真正的 BIOS→OS 交权。超时则强制 `(sup \| OS) & ~BIOS` 并记 `kFlagHandoffForced`——**这一条是「每次都能开机」的关键：绝不等一个可能永远不来的握手** |
| 4 | 写 `cap+8`（USBLEGCTLSTS）：清掉全部 SMI enable 位，W1C 清事件位 | BIOS 不再插手端口状态；事件位不清会在下一次交权时立刻重触发 SMI |
| 5 | 清 `USBCMD.RS/INTE/EWE/HSEE`，等 `USBSTS.HCH`（默认 250 ms） | 先把控制器停下来，否则 HCRST 会被进行中的传输拖住 |
| 6 | 先写 `CRCR = 0`，再 `HCRST`，等 HCRST 自清 + CNR 清（默认 1000 ms，最多重试 2 次） | 清 CRCR 是为了让固件遗留的命令环指针不会在被 reset 前又被取用 |
| 7 | 仅在确实 halt 或 reset 成功时，清 `CRCR` / `DCBAAP` / `CONFIG` | 不满足条件就清，会在 RS=1 时触发 Host System Error。这是刻意加的守卫 |
| 8 | W1C 清 `USBSTS` 与每个端口的 change bits；端口 PLS 处于 Polling/Recovery/HotReset/Compliance/Loopback 时，带 LWS 写回 RxDetect | 让 Apple 驱动看到「没插设备、端口在等」的干净初态，而不是半个复位 |
| 9 | 走 PCI cap 链，清 MSI `MC.bit0` 与 MSI-X `MC.bit15` | 让 Apple 驱动自己按它的策略重新使能中断，而不是继承固件的使能状态 |

写端口时只写 `PP | 0xC000 | WCE | WDE | WOE | change bits`，**PLS 字段只有在 `LWS=1` 时才携带**——这是规范要求，违反它会把端口状态机写坏。

全部关闭时典型耗时 **2 ms 量级**，最坏 ~1.3 s（全部超时叠加），并且 `max_total_ms` 硬上限 5000 ms。

### 顺带修掉的两个真实 bug

1. **`Leaf_Methods.cpp` 的 `TakeOwnershipFromBios()` 偏移错了一个 dword。**
   `_pUSBLegSup = (uint32_t volatile*)iter` 指向的是能力结构**起点**（capId/next 那个 dword），于是 `Read32Reg(_pUSBLegSup)` 读的不是 USBLEGSUP，`Write32Reg(_pUSBLegSup+1, ...)` 写进去的是 `(ctl & 0xE1FEE) | (7<<29)`——这个 mask 不含 bit16/bit24，等于**强行清掉 BIOS_OWNED 却从不清 OS_OWNED**，真正的 CTLSTS 一次都没被写过。按 xHCI 规范（与 Linux `pci-quirks.c`、QEMU `xhci-ext-caps` 一致）USBLEGSUP 在 cap+4、USBLEGCTLSTS 在 cap+8。
   → preflight 用正确偏移。

2. **`V1Pure.cpp` 的 `configWrite16(kIOPCIConfigCommand, kIOPCICommandMemorySpace)` 是写而不是或**，会顺手清掉 bus-master（GUX 自己的注释都承认 "This also disables bus-mastering. Bios may still own the xHC. Is that a problem?"）。
   → 改成读-改-写 `| MemorySpace | BusMaster`。

---

## 3. 方案 B：独立 kext（推荐）

`kext/` 下是完整源码：`IOService` 子类，`probe()` 只认 `vendor == 0x1022`，`start()` 跑 preflight 后**故意 `return false`**——IOKit 会摘掉这个候选者并启动下一个，也就是 AppleUSBXHCIPCI。

- 不继承 `IOUSBController`，所以 10.10 头编译出来的 vtable 永远不会挡在新内核前面；
- `probe()` 里不碰任何 MMIO（probe 阶段还没有串行化保证），所有硬件动作在 `start()`；
- 无论成功、跳过还是失败都返回 false，不持锁、不注册服务、不占设备。

### 编译（需要 macOS + Xcode 命令行工具）

```bash
cd kext
./build.sh
# built: .../AMDXHCIPreflight.kext
```

`build.sh` 用 `clang++ -mkernel -Xlinker -kext` 直接出 MH_KEXT_BUNDLE，最后 ad-hoc 签名。

> ⚠️ 这个脚本是在 Windows 上写的，**没有在 macOS 上跑过**。如果链接阶段报 `-lkmod` / `libcc_kext` 找不到，走 Xcode 路线：新建 Kernel Extension target，把 `AMDXHCIPreflight.cpp` 加进去，`GCC_ENABLE_KERNEL_DEVELOPMENT = YES`，`MODULE_NAME` 留空，签名用 `-`。方案 A 不需要编译环境以外的任何东西，风险更低。

### 安装（OpenCore）

```xml
<key>Kernel</key><dict><key>Add</key><array><dict>
    <key>BundlePath</key>    <string>AMDXHCIPreflight.kext</string>
    <key>ExecutablePath</key><string>Contents/MacOS/AMDXHCIPreflight</string>
    <key>PlistPath</key>     <string>Contents/Info.plist</string>
    <key>Enabled</key>       <true/>
</dict></array></dict>
```

拷到 `EFI/OC/Kexts/AMDXHCIPreflight.kext`。`MinKernel` / `MaxKernel` 留空。

> 必须走 OpenCore 注入（进 kernelcache、开机早期加载）。放进 `/Library/Extensions` 是**开机后**才加载的，对开机卡住没有帮助。

### 同时要做的

**删掉 GUX-RyzenXHCIFix。** 它现在唯一的作用（早期 init 后退出）已经被 preflight 取代，而它还会清掉 bus-master。留着它 = 两个 score 10000 的候选者互相争抢，比原来更糟。

---

## 4. 方案 A：给 fork 打补丁

```bash
git clone https://github.com/RattletraPM/GUX-RyzenXHCIFix
cd GUX-RyzenXHCIFix
git apply /path/to/amdxhci-preflight/gux/GUX-Preflight.patch
make            # 需要 macOS + Xcode
```

或者不用 patch，直接跑生成器（幂等，锚点命中数不等于 1 就中止）：

```bash
python3 gux/gen_patch.py /path/to/GUX-RyzenXHCIFix
```

补丁对 `GenericUSBXHCI/V1Pure.cpp` 只做四件事：

1. `#include "AMDXHCIPreflight.h"`（并把 `AMDXHCIPreflight.h` 放进 `GenericUSBXHCI/`，头文件不需要加进 xcodeproj）；
2. 插入 adapter（`amdxhci_pf_*` 回调 + boot-arg 解析 + 结果写进 provider 属性）；
3. `configWrite16` 改成读-改-写，紧跟 `amdxhci_pf_run()` + `UIMFinalize()` + `return kIOReturnUnsupported;`（函数其余部分变成死代码，已加注释说明）；
4. 把 `b7c0850` 注释掉的大括号复原——现在不需要靠「故意失败」来退出了。

`Info.plist` 的 `IOProbeScore` 从 `1` 改成 `10000`，消除第 6 条竞态。

---

## 5. 验证

**编译前**先跑核心逻辑的宿主侧单测（Windows/Linux 都行，用 WSL 的 g++ 或 clang++）：

```bash
cd core
bash build_and_test.sh     # g++/clang++ × c++98/11/17 + asan/ubsan，当前 82 checks / 0 failures
```

kext 源码不用 Mac 也能类型检查（用 `kext/syntax-check/stub/` 里那套假 IOKit 头）：

```bash
cd kext/syntax-check
./check.sh                 # c++11 / c++14 / c++17 三组 clang++ 编译，当前全部 PASS
```

补丁本身也可以先空跑一遍确认锚点没漂移：

```bash
cd GUX-RyzenXHCIFix
git apply --check /path/to/amdxhci-preflight/gux/GUX-Preflight.patch   # 无输出 = 可应用
```

这个测试用一台模拟的 AMD xHC 跑了 15 个场景：健康、幻影控制器（全 1 / 全 0）、BAR 过小、无 LEGSUP、BIOS 不肯放手、reset 永不完成、reset 慢、CNR 慢、CNR 永不清、强制 RxDetect、全开关关闭、32 端口、halt+reset 双卡死。最后两个专门验证**卡死时也绝不写 ring 寄存器**。

**开机后**：

```bash
# 1) preflight 到底跑没跑，结果是什么
ioreg -l -w0 | grep -i amdxhci
#   "amdxhci-preflight" = "ok 0x1022:0x43ee cap=0x40 ver=0x0110 ports=8 slots=64 legs=0x110 ho=ok rst=ok/20ms rx=0x00000024 msi=3 t=20 flags=0x3d80"

# 2) 内核日志
log show --last boot --predicate 'eventMessage CONTAINS "AMDXHCIPreflight"' --info

# 3) kext 是否加载（方案 B）
kmutil showloaded --list-only | grep -i amdxhci
```

`flags` 的解读：

| 位 | 名称 | 含义 |
|---|---|---|
| `0x0001` | `kFlagPhantom` | 判为幻影控制器，未做任何写入 |
| `0x0002` | `kFlagBailSafe` | 走了保守路径 |
| `0x0004` | `kFlagRegFault` | MMIO 读回全 1，寄存器不可达 |
| `0x0008` | `kFlagCnrTimeout` | CNR 在超时内没清（仍继续） |
| `0x0010` | `kFlagLegacyMissing` | XECP 链里没有 LEGSUP 能力 |
| `0x0020` | `kFlagHandoffForced` | BIOS 不放手，**强制**夺权（正常表现，不是错误） |
| `0x0040` | `kFlagHandoffTimeout` | 交权超时 |
| `0x0080` | `kFlagHalted` | 控制器已停 |
| `0x0100` | `kFlagResetOk` | HCRST 成功 |
| `0x0200` | `kFlagResetTimeout` | HCRST 未在超时内自清 |
| `0x0400` | `kFlagRingsZeroed` | CRCR/DCBAAP/CONFIG 已清零 |
| `0x0800` | `kFlagPortsCleared` | 端口 change bits 已清 |
| `0x1000` | `kFlagRxDetectForced` | 有端口被强制拉回 RxDetect |
| `0x2000` | `kFlagMsiOff` | MSI/MSI-X 使能位已关 |
| `0x4000` | `kFlagSkew` | 设备/版本自洽性检查异常 |

**判定标准**：连续冷启动 20 次，`ho=` 为 `ok` 或 `forced`、`rst=` 为 `ok`、`t=` 在几十毫秒内，且 20 次全部进系统。

---

## 6. 回滚

| 手段 | 做法 |
|---|---|
| 最快 | boot-arg 加 `-amdxhci_off`：preflight 完全跳过，驱动行为回到「什么都不做，直接让位」 |
| 方案 B | 从 `Kernel → Add` 里删掉该项（或 `Enabled=false`），GUX 保持删除状态 |
| 方案 A | `git apply -R` 撤销补丁，或把 `IOProbeScore` 改回 `1` |

加 `-amdxhci_off` 后如果开机恢复正常，说明卡住确实来自固件遗留状态；如果反而变差，说明问题在别处（见下节）。

---

## 7. 诊断开关

| boot-arg | 作用 |
|---|---|
| `-amdxhci_off` | 完全跳过 preflight |
| `-amdxhci_nohandoff` | 不做 LEGSUP 交权（排查「是不是交权本身触发了问题」） |
| `-amdxhci_noreset` | 不做 HCRST |
| `-amdxhci_noports` | 不碰端口寄存器 |
| `-amdxhci_nomsi` | 不动 MSI/MSI-X |
| `-amdxhci_rxdetect_all` | 所有端口一律强制回 RxDetect（有些 AMD 板子必须这样） |
| `amdxhci_cnr_ms=N` | CNR 等待上限，默认 1200 |
| `amdxhci_reset_ms=N` | HCRST 等待上限，默认 1000 |
| `amdxhci_handoff_ms=N` | 交权等待上限，默认 1000 |
| `amdxhci_retries=N` | HCRST 重试次数，默认 2（上限 8） |
| `amdxhci_log=N` | 1 只打一行结果，2 打每一步，0 静默 |

排查方法：一次只加一个 `-amdxhci_no*`，看 `amdxhci-preflight` 那一行的哪个字段变了。

---

## 8. 已知边界

- **这台机器上没有 macOS**，kext 只能在这里写、在 WSL 里对核心逻辑做模拟单测，**没有在真实 AMD 硬件上验证过**。方案 A 的补丁也只验证了锚点命中与 ID 自洽性，没有真正 `xcodebuild`。
- 不处理 ACPI 层的 `_PS0/_PS3` 或 `XHCI` 的 `_DSM`——Apple 驱动会自己走电源管理。如果卡住发生在 `AppleUSBXHCIPCI` 已经接管之后（USB 设备树建完再卡），问题不在这里。
- `DBOFF` / `RTSOFF` 是字节还是 DWORD 偏移在 GUX 里用法不一致，preflight **刻意完全不用这两个寄存器**，只用 `CapLength`（字节偏移）和 `HCCParams.XECP`（DWORD 偏移）。
- 如果 `flags` 里 `kFlagPhantom` 出现在一个你确定存在的控制器上，说明 BAR0 没被正确分配（BIOS/OC 的 `ResizeAppleGpuBars`、Above4G 或 ACPI `_CRS` 问题），preflight 选择什么都不写——先去修 ACPI/PCI 资源分配。
- 不碰 `XHCI_HCC_AC64` 之外的 64 位地址能力判断，也不做 `PageSize` 调整：Apple 驱动会重做。

---

## 9. 文件清单

```
amdxhci-preflight/
├── core/
│   ├── AMDXHCIPreflight.h      # header-only 状态归一器（无 IOKit 依赖）
│   ├── test_amdxhci.cpp        # 模拟 AMD xHC 的单测，82 checks
│   └── build_and_test.sh       # 6 组编译 + asan/ubsan
├── gux/
│   ├── GUX-Preflight.patch     # 方案 A：打到 GUX-RyzenXHCIFix
│   └── gen_patch.py            # 同一件事的生成器（幂等、严格锚点）
├── kext/
│   ├── AMDXHCIPreflight.cpp    # 方案 B：IOService 子类，随时让位
│   ├── Info.plist
│   ├── build.sh                # macOS：clang++ -mkernel -Xlinker -kext
│   └── syntax-check/           # 非 macOS 环境下的类型检查（假 IOKit 头）
│       ├── check.sh
│       └── stub/
└── README.md
```

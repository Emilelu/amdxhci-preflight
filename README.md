# AMD xHCI 开机卡住修复：把「碰运气」的退出改成确定性的交接

针对 [RattletraPM/GUX-RyzenXHCIFix](https://github.com/RattletraPM/GUX-RyzenXHCIFix) 那套「让 GUX 在早期 init 后故意失败退出」的做法。

> **症状的版本范围比这个 fork 声称的宽得多**：Catalina 起就有，一直到现在。实测记录——Catalina 上**装不装这个 fork 都会卡**（只有 SSDT 禁用一个 XHCI 控制器才进得去），Big Sur 以上装 fork 是偶发卡。也就是说卡死的根源不在 macOS 版本，也不在这个 fork：是固件/BIOS 交接之后 xHC 留在了一个 Apple 驱动无法可靠复位干净的状态，fork 里那段「早期 init 后退出」只是**碰巧**动到了几个寄存器，所以时灵时不灵。

---

## TL;DR

两条路径，选一条，**不要同时装**：

| | 做法 | 适用 |
|---|---|---|
| **A** | 把 `gux/GUX-Preflight.patch` 打进 GUX-RyzenXHCIFix，替换掉它原来那套「故意失败退出」的把戏 | 你已经在用这个 fork，想最小改动 |
| **B**（推荐） | 删掉 GUX-RyzenXHCIFix，装独立的 `AMDXHCIPreflight.kext` | 干净、不继承 10.10 的 vtable、可与任何 USB 驱动共存 |

两者共用同一份 preflight 实现（`core/AMDXHCIPreflight.h`），行为完全一致。

> **在 macOS 26.7.3 (Tahoe) 上实测：方案 A 走不通，只能用方案 B。** 原因和证据见下面第 0 节。

---

## 0. 26.7.3 上实测的兼容性结论（先看这个）

下面每一条都是在一台真实 macOS 26.7.3 (25G342, x86_64) 上跑出来的，不是推测。

**0.1 GUX 声明的依赖在 Tahoe 上不存在**

fork 的 `Info.plist` 写着 `com.apple.iokit.IOUSBFamily 5.0.0a1`。26.7.3 上实际的 USB 栈是：

```
com.apple.iokit.IOUSBHostFamily (1.2)          ← 改名后的 IOUSBFamily
com.apple.driver.usb.AppleUSBXHCI (1.2)
com.apple.driver.usb.AppleUSBXHCIPCI (1.2)
```

`IOUSBFamily` 这个名字已经没了，加载时报：

```
Unable to resolve dependencies: 'net.osx86.kexts.GenericUSBXHCI'
names a dependency on 'com.apple.iokit.IOUSBFamily', which was not found.
```

**0.2 就算把依赖名改成 IOUSBHostFamily，符号也绑不上**

GUX 全部实现建立在 `class GenericUSBXHCI : public IOUSBControllerV3` 之上，而 `IOUSBControllerV3` 的声明来自仓库自带的 `IOKit-10.10/IOUSBControllerV3.h`。Tahoe 的内核集合里既没有导出的 `IOUSBControllerV3`，也没有它依赖的 `IOUSBControllerIsochListElement::gMetaClass`：

```
nm /System/Library/KernelCollections/BootKernelExtensions.kc | grep -c IOUSBControllerV3
0

kmutil load -p GenericUSBXHCI.kext:
Cannot find symbol for metaclass pointed to by '__ZN21GenericUSBXHCIIsochTD10superClassE'.
Expected symbol '__ZN31IOUSBControllerIsochListElement10gMetaClassE'
to be defined in another kext
```

也就是说：**这个 kext 在 Tahoe 上根本加载不进去**。它还能在某些机器上「起作用」，只可能是因为 OpenCore 重建内核集合时把所有 kext 一起重链，而那时它用的是 10.10 的 vtable 布局 —— 这恰好解释了为什么它在 Sonoma 之后开始偶发卡死：结构体/vtable 每版都在长，偏移对不上时，写进去的就是垃圾。

**0.3 独立 kext 在 26.7.3 上是通的**

`kext/build.sh` 产出的 `AMDXHCIPreflight.kext` 在这台机器上完成了完整的内核集合构建，唯一剩下的报错是策略层：

```
Extension with identifiers com.yg.AMDXHCIPreflight not approved to load.
Please approve using System Settings.
```

这句报错出现在**依赖解析与符号绑定全部通过之后**，所以可以确认二进制结构、依赖表、kmod 入口都是对的；它只依赖 `com.apple.kpi.iokit` / `com.apple.kpi.libkern` / `com.apple.iokit.IOPCIFamily`，这三者在每个版本都稳定。真机上用 OpenCore 注入时 kext 在 UAKEL 策略之前就进了内核集合，不需要这个「批准」。

> 两处非直觉的链接细节（已在 `kext/build.sh` 里处理好，否则会卡在 `kexts must have a _kmod_info symbol` 和 `Failed to bind '__realmain'`）：
> 1. `mach/kmod.h` 里的 `_kmod_info` 必须由 kext 自己定义 —— `libkmod.a` 的 `c_start.o` 里它只是个未定义引用；
> 2. `__realmain` 也必须自己定义：`c_start.o` 的 `__start` 是 `rax = __realmain; jmp [rax]`，即它读一个「首字段为入口函数指针」的结构。
> 另外 plist 里必须声明 `com.apple.iokit.IOPCIFamily`，否则 `IOPCIDevice::metaClass` 绑定失败。

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

### 1.1 为什么只有 AMD 会这样

Intel 机器的 UEFI 在 `ExitBootServices` 前后会把 xHC 干净地交给 OS（`USBLEGSUP` 交接、SMI 关掉、端口回 RxDetect），Apple 驱动就是按这个前提写的 —— 它自己**不检查也不处理** `USBLEGSUP`。AMD 主板（尤其是 AM4/AM5 的 ASUS、Gigabyte）经常做不到：

- `BIOS_OWNED` 留着、USB SMI 使能位留着 → 开机早期 SMI 风暴，控制器在 Apple 看来像「死了」
- 端口被固件留在 `Polling` / `Compliance` / `U3`，`USBSTS.CNR` 不清 → Apple 的 HCRST 超时，卡在这一步
- 寄存器里还残留上一位主人的 `CRCR` / `DCBAAP` 指向已经不属于 OS 的内存
- 部分芯片组 xHC 没有可用 MSI，中断分配失败

这四条正是 §2 表格里第 2–7 步逐条处理的，也是「不同 macOS 版本都卡、不同主板卡的表现还不一样」的原因 —— 卡的位置取决于固件留下了哪一种脏状态。

## 2. 修复方案：把退出前的动作补全

`core/AMDXHCIPreflight.h` 是一个 header-only、不依赖 IOKit 的状态归一器。它在 fork 里替换掉那段假退出，在独立 kext 里作为 `start()` 的主体。

执行顺序（每一步都有界，任何一步超时都**只打 flag 不中断**，绝不会把开机挂死）：

| 步 | 动作 | 为什么 |
|---|---|---|
| 0 | `Classify()`：BAR < 4K、cap 区读回全 1/全 0、cap_length / HCI version / ports / slots 越界 → 判为幻影控制器 | **零写入**直接退出。AMD 平台上空的 PCI 槽会被类匹配到，往幻影设备写寄存器就是挂死 |
| 1 | PCI `Command |= MemorySpace\|BusMaster`；PMCSR 低 2 位非 0 则写 0 进 D0 | 固件常把设备留在 D3 或关着 bus-master，此时碰 MMIO 本身就是错的 |
| 2 | 等 `USBSTS.CNR` 清零（默认 1200 ms） | CNR=1 时 HCRST 无效。超时不清也不放弃，记 `kFlagCnrTimeout` 继续 |
| 3 | 走 XECP 链找 capId=1；在能力结构**第一个 dword**（`cap+0`，与 capId/next 同字）写 `OS_OWNED`，轮询等 `BIOS_OWNED` 清（默认 1000 ms） | 这才是真正的 BIOS→OS 交权。超时则强制 `(sup \| OS) & ~BIOS` 并记 `kFlagHandoffForced`——**这一条是「每次都能开机」的关键：绝不等一个可能永远不来的握手** |
| 4 | 写 `cap+4`（USBLEGCTLSTS）：保留只读位（1..3、5..12、17..19），其余写 0（含 bit0 总 SMI 使能），W1C 清 29..31 | 与 Linux `pci-quirks.c` 同一套写法：BIOS 不再插手端口状态，事件位不 W1C 会在下次交权时立刻重触发 SMI |
| 5 | 清 `USBCMD.RS/INTE/EWE/HSEE`，等 `USBSTS.HCH`（默认 250 ms） | 先把控制器停下来，否则 HCRST 会被进行中的传输拖住 |
| 6 | 先写 `CRCR = 0`，再 `HCRST`，等 HCRST 自清 + CNR 清（默认 1000 ms，最多重试 2 次） | 清 CRCR 是为了让固件遗留的命令环指针不会在被 reset 前又被取用 |
| 7 | 仅在确实 halt 或 reset 成功时，清 `CRCR` / `DCBAAP` / `CONFIG` | 不满足条件就清，会在 RS=1 时触发 Host System Error。这是刻意加的守卫 |
| 8 | W1C 清 `USBSTS` 与每个端口的 change bits；端口 PLS 处于 Polling/Recovery/HotReset/Compliance/Loopback 时，带 LWS 写回 RxDetect | 让 Apple 驱动看到「没插设备、端口在等」的干净初态，而不是半个复位 |
| 9 | 走 PCI cap 链，清 MSI `MC.bit0` 与 MSI-X `MC.bit15` | 让 Apple 驱动自己按它的策略重新使能中断，而不是继承固件的使能状态 |

写端口时只写 `PP | 0xC000 | WCE | WDE | WOE | change bits`，**PLS 字段只有在 `LWS=1` 时才携带**——这是规范要求，违反它会把端口状态机写坏。

全部关闭时典型耗时 **2 ms 量级**，最坏 ~1.3 s（全部超时叠加），并且 `max_total_ms` 硬上限 5000 ms。

### 顺带修掉的真实问题

1. **`configWrite16(kIOPCIConfigCommand, kIOPCICommandMemorySpace)` 是写而不是或**，会顺手清掉 bus-master（GUX 自己的注释都承认 "This also disables bus-mastering. Bios may still own the xHC. Is that a problem?"）。
   → 改成读-改-写 `| MemorySpace | BusMaster`。

2. **BIOS→OS 交权在这个 fork 里从来没有执行过**（`TakeOwnershipFromBios()` 在早退点之后）。preflight 把它补上，并把 GUX 原来 100 ms 的等待换成 1000 ms、且超时后**强制清 semaphore**——GUX 的做法是超时只 `IOLog` 一句然后继续，等于把「BIOS 还握着控制器」这个状态原样留给下一个驱动。

3. **勘误（重要）**：本文早先版本写过「GUX 的 LEGSUP 偏移错了一个 dword」，那是**错的**。复核 Linux `drivers/usb/host/xhci-ext-caps.h` 后确认：
   ```c
   #define XHCI_LEGACY_SUPPORT_OFFSET	(0x00)	/* USBLEGSUP */
   #define XHCI_LEGACY_CONTROL_OFFSET	(0x04)	/* USBLEGCTLSTS */
   ```
   即 `BIOS_OWNED`/`OS_OWNED` 与 capId/next **共用结构第一个 dword**，GUX 的 `Read32Reg(_pUSBLegSup)` / `Write32Reg(_pUSBLegSup+1, …)` 是对的；同一文件里 `offset += next << 2` 也证实 XECP 的 `next` 是 **DWORD 增量**（与 GUX 的 `iter + iter->next` 一致）。preflight 早期版本按错误理解写成 `cap+4` / `cap+8`，已修正并有单测锁住（`legs=0x110` 那行的报告值即结构偏移）。
   → 结论：**GUX 的寄存器偏移没错，错的是它根本没走到那段代码。**

---

## 3. 方案 B：独立 kext（推荐）

`kext/` 下是完整源码：`IOService` 子类，`probe()` 只认 `vendor == 0x1022`，`start()` 跑 preflight 后**故意 `return false`**——IOKit 会摘掉这个候选者并启动下一个，也就是 AppleUSBXHCIPCI。

- 不继承 `IOUSBController`，所以 10.10 头编译出来的 vtable 永远不会挡在新内核前面；
- `probe()` 里不碰任何 MMIO（probe 阶段还没有串行化保证），所有硬件动作在 `start()`；
- 无论成功、跳过还是失败都返回 false，不持锁、不注册服务、不占设备。

### 编译（macOS，**只要命令行工具，不需要 Xcode.app**）

```bash
./build-all.sh            # 单测 + 两个 kext，产物落 out/
./build-all.sh --core-only   # 只跑宿主侧单测（Linux 也能跑）
```

或者单独编：

```bash
cd kext && ./build.sh               # -> kext/AMDXHCIPreflight.kext
cd ../gux && ./build_gux.sh <源码树> <输出目录>   # 不用 xcodebuild 编 GUX
```

`kext/build.sh` 用 `clang++ -mkernel -Xlinker -kext` 直接出 MH_KEXT_BUNDLE，`gux/build_gux.sh` 同样绕过 xcodebuild 逐文件编 26 个源文件。两者都做 ad-hoc 签名。

> 已在 macOS 26.7.3 + Apple clang 21.0.0 + Command Line Tools 上实测跑通：单测 7/7 PASS，两个 kext 都是 `MH_KEXT_BUNDLE x86_64`，`plutil -lint` 通过。产物在 `out/`。
> 若你有完整 Xcode，`build-all.sh` 会自动改用 `xcodebuild -configuration "El Capitan"`（fork 自带的工程），结果等价。

### 安装（OpenCore）

```xml
<key>Kernel</key><dict><key>Add</key><array><dict>
    <key>BundlePath</key>    <string>AMDXHCIPreflight.kext</string>
    <key>ExecutablePath</key><string>Contents/MacOS/AMDXHCIPreflight</string>
    <key>PlistPath</key>     <string>Contents/Info.plist</string>
    <key>Enabled</key>       <true/>
</dict></array></dict>
```

拷到 `EFI/OC/Kexts/AMDXHCIPreflight.kext`，`MinKernel` 填 `19.0.0`（Catalina 起），`MaxKernel` 留空。

> **兼容范围：Catalina 10.15 → Tahoe 26，同一个 x86_64 二进制。** 这个 bug 是 AMD 平台特有的（Intel 机型不出现），且与 macOS 版本无关 —— Catalina 上装不装原版 fork 都卡，所以没有理由把它限制在 Sonoma 之上。代码只用 `IOService` / `IOPCIDevice` / `IOMemoryMap` / `IOLog` / `IOSleep` 和自己定义的一套 xHCI 寄存器常量，不继承任何 USB 类，因此不随 USB 栈改版错位。版本门槛全在 plist 里：`com.apple.kpi.*` 声明 `12.0.0`（10.9 起就没变过），`com.apple.iokit.IOPCIFamily` 声明 **`2.6`** 而不是编译机上的 `2.9` —— 加载器要求「库的 compatible 版 ≤ 声明值 ≤ 库的 CFBundleVersion」，取低界才能一个二进制通吃（Catalina 的 `IOPCIFamily` 是 2.8 一代，同样满足）。Mojave 及更早没验过，真要用就把 `MinKernel` 改成 `18.0.0` / `17.0.0`。

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

### 已经在真机上验过的（macOS 26.7.3 / Apple clang 21 / 只有 CLT）

| 项目 | 结果 |
|---|---|
| `core/build_and_test.sh` | 7/7 PASS（g++、clang++ × c++98/11/17 + asan/ubsan，100 checks） |
| `kext/syntax-check/check.sh` | c++11 / c++14 / c++17 全 PASS |
| `./build-all.sh`（无 Xcode） | 两个 kext 都产出 `MH_KEXT_BUNDLE x86_64`，`plutil -lint` OK，adhoc 签名成功 |
| `AMDXHCIPreflight.kext` 的内核集合构建 | 依赖解析 + 符号绑定全部通过；剩下的只是 UAKEL「未批准加载」策略拦截 |
| 入口点符号（`nm -m`） | `_kmod_info` @ `__DATA` 0x4998、`__realmain` @ `__DATA` 0x4990，都是**已定义**（不是 U），`_pf_realmain` 在 `__TEXT` 0x3450 |
| 构造器接线（`otool -v -s __DATA __mod_init_func`） | section 里**恰好一个**指针 `0x33f0` → `__GLOBAL__sub_I_AMDXHCIPreflight.cpp`，即类注册的静态构造函数；`pf_realmain` 就是遍历这一段把它跑起来的 |
| 类符号（`nm -m`） | `__ZN23com_yg_AMDXHCIPreflight10gMetaClassE`、`…9metaClassE`、`__ZTV…`（vtable）、`…5probeE…`、`…5startE…` 全部已定义 |
| `GenericUSBXHCI.kext`（打完补丁） | 构建成功，但**加载失败**：`Cannot find symbol for metaclass ... 'IOUSBControllerIsochListElement::gMetaClass'` —— 见第 0.2 节，这条路在 Tahoe 上封死 |

### 一个还没验的点：加载时构造器是否真的被调用

静态接线已经验完（上表最后三行）：入口点符号存在、`__mod_init_func` 恰好指向 `__GLOBAL__sub_I_AMDXHCIPreflight.cpp`、我们自己的 `gMetaClass` / vtable 都已定义。剩下唯一没验的是**动态那一步**——内核集合构建器会不会在加载时通过 `__realmain` 调它——而这只能在被 UAKEL 放行之后才看得到。

判断方法（装上后一眼就能看出）：

```bash
# 1) kext 加载了没
kmutil showloaded --list-only | grep -i amdxhci

# 2) 类注册了没（关键）
ioreg -c com_yg_AMDXHCIPreflight -l | head

# 3) 控制器上有没有出现我们的属性（说明 start() 真的被调用了）
ioreg -rc IOPCIDevice -l | grep -A1 amdxhci-preflight
```

第 2 条没有输出 = 类没注册，personality 匹配不会发生，preflight 不会跑。静态检查已经确认入口点和构造器接线都是对的，所以真出现这种情况，先怀疑**加载时机**（比如 kext 没进 kernelcache、被别的候选者抢先），再怀疑链接。第 3 条有 `amdxhci-preflight` 那行字符串，就说明整条链路（匹配 → start → preflight → 交棒）都通了。


**编译前**先跑核心逻辑的宿主侧单测（Windows/Linux 都行，用 WSL 的 g++ 或 clang++）：

```bash
cd core
bash build_and_test.sh     # g++/clang++ × c++98/11/17 + asan/ubsan，当前 100 checks / 0 failures
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

这个测试用一台模拟的 AMD xHC 跑了 18 个场景：健康、幻影控制器（全 1 / 全 0）、BAR 过小、无 LEGSUP、BIOS 不肯放手、reset 永不完成、reset 慢、CNR 慢、CNR 永不清、强制 RxDetect、全开关关闭、干跑（全开也只读）、32 端口、halt+reset 双卡死、FLR 成功/不支持/永不完成。最后几项专门验证**卡死时也绝不写 ring 寄存器**、**干跑时一个字节都不写**。模拟器里的 LEGSUP 是按其真实布局摆的：`cap+0` 同时装 capId=1 和 `BIOS_OWNED`，`cap+4` 是 `USBLEGCTLSTS`——偏移写错会立刻在第 3 步行权断言上炸出来。

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
| `-amdxhci_dryrun` | 只分类、只报告，零 MMIO 写、零 PCI 配置写 |
| `-amdxhci_anyvendor` | 允许在非 `0x1022` 控制器上匹配；只配合 dryrun 用来验链路 |
| `-amdxhci_flr` | 先做 PCIe Function Level Reset（把整个 function 打回热复位状态） |
| `-amdxhci_noflr` | 明确不做 FLR（默认就是不做） |
| `amdxhci_flr_ms=N` | 等 FLR 完成的超时，默认 200 |
| `amdxhci_cnr_ms=N` | CNR 等待上限，默认 1200 |
| `amdxhci_reset_ms=N` | HCRST 等待上限，默认 1000 |
| `amdxhci_handoff_ms=N` | 交权等待上限，默认 1000 |
| `amdxhci_retries=N` | HCRST 重试次数，默认 2（上限 8） |
| `amdxhci_log=N` | 1 只打一行结果，2 打每一步，0 静默 |

排查方法：一次只加一个 `-amdxhci_no*`，看 `amdxhci-preflight` 那一行的哪个字段变了。

`-amdxhci_dryrun` + `-amdxhci_anyvendor` 是「先证明链路、再动硬件」用的：`Run()` 自己在内部把所有写路径关掉（不是靠调用方自觉），所以报告里只有分类结果、`flags` 带 `dry` 位、控制器上一个字节都没被写过。这两个参数让 kext 在任何一台 Intel 机上也能跑一遍，从而在把它放到 AMD 机之前就确认「入口点 → `OSMetaClass` 注册 → personality 匹配 → `start()` → 报告」全是通的。

**`-amdxhci_flr` 是留给「默认参数仍然偶尔卡」那台机器的下一张牌**，默认关闭。`HCRST` 只复位 xHCI 核，不复位这个 PCI function 自己持有的东西（DMA 绑定、MSI 表、端口供电状态）；FLR 是唯一能把整个 function 打回热复位状态的复位，也是别的步骤都不会做的那一步。不开它是因为少数固件会「声称支持 FLR，然后真按下去就再也回不来」——没有那台机器可测的情况下不该默认按这个按钮。开启后报告里多一段 `flr=ok` / `flr=fail`，属性里是 `"amdxhci-flr" = "ok" | "timeout" | "not-supported" | "disabled"`。

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
├── build-all.sh                # 一键：单测 + 两个 kext（CLT 即可，不需要 Xcode.app）
├── core/
│   ├── AMDXHCIPreflight.h      # header-only 状态归一器（无 IOKit 依赖）
│   ├── test_amdxhci.cpp        # 模拟 AMD xHC 的单测，82 checks
│   └── build_and_test.sh       # 6 组编译 + asan/ubsan
├── gux/
│   ├── GUX-Preflight.patch     # 方案 A：打到 GUX-RyzenXHCIFix
│   ├── build_gux.sh            # 无 xcodebuild 编 GUX（模板展开 / CRLF / kmod 入口）
│   └── gen_patch.py            # 同一件事的生成器（幂等、严格锚点）
├── kext/
│   ├── AMDXHCIPreflight.cpp    # 方案 B：IOService 子类，随时让位
│   ├── Info.plist
│   ├── build.sh                # macOS：clang++ -mkernel -Xlinker -kext
│   └── syntax-check/           # 非 macOS 环境下的类型检查（假 IOKit 头）
│       ├── check.sh
│       └── stub/
├── out/                        # 26.7.3 上真实构建出来的产物
│   ├── AMDXHCIPreflight.kext
│   ├── GenericUSBXHCI.kext
│   └── build-report.txt
└── README.md
```

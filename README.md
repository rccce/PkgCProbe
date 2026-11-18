# PkgCProbe：查 CPU 封装为什么停在浅层 C-state（诊断）

## 目前的结论

- SATA 控制器一直上报 10.2 µs 的延迟要求（LTR），把封装限制在 PC2。ApplyMask 4（不计 SATA 的 LTR）后能进 PC3，已验证。
- PC6 以上一直是 0。每次往深处走，都因为 I/O 总线（IOSF）没能停下而退回 PC3。芯片组所有 LTR 都不计时也一样，所以不是 LTR 的问题。
- 已排除：DMI 链路两端的 ASPM L1、独显口 PEG0、RTL8111 链路 L1（r5 读到网卡芯片自己的 ASPM_en 和 ClkReqEn 也都是 1）、关屏（pkgc2：关屏后显示引擎的唤醒归零，仍然只有 PC3）。
- r5（pkgc6）：通过 AHCI 驱动打开 SATA ALPM 成功（ALPE = 1），但 120 次采样 SATA 链路全部停在 Active，主动请求 Slumber 也不接受。推断：这块 SATA 盘不接受主机发起的链路省电。这条线到此为止，r6 去掉了 ALPM 部分。
- r5 的只读概览发现：Wi-Fi 卡（AX210，06:00.0）这一侧的 ASPM 是关的（LNKCTL 0x0140），根端口 1c.7 那一侧开着 L1。它是唯一一条有设备在工作、却没有 L1 的 PCIe 链路。原因在 AirportItlwm 源码 `itlwm/hal_iwx/ItlIwx.cpp` 的 `iwx_apm_config()`：每次启动网卡都把 L0s 和 L1 一起清掉（注释写着 “Disable L0s and L1s in PCIE register because we don't support ASPM now”）。Linux 的 iwlwifi 只关 L0s，不动 L1。
- 另外两个可疑点：1c.0 是一个没有设备、没有链路的根端口，LNKSTS 里“正在训练”位一直是 1；芯片组 xHCI 上 4 个 USB 设备（外接 USB 2.0 Hub、Synaptics 06cb:0078（推断是指纹模块，macOS 没有驱动）、蓝牙、摄像头）每次读都在 U0（工作中）。

## 它做什么

驱动启动 StartDelay 秒后开始（本目录的 Info.plist 是 300 秒，即 5 分钟）。

先只读一遍（同 r5），另加：

- 根端口没有设备时，多读插槽寄存器（SLTCAP、SLTCTL、SLTSTA），LNKSTS 多解出“正在训练”位。
- Wi-Fi 卡和它上面根端口的链路详情：ASPM、L1 退出延迟、卡能接受的 L1 延迟、LTR。
- 列出空闲根端口（下面没有设备、链路没起来、没被禁用）。

然后分 6 段测量，每段先等 2 秒再测 15 秒，一共约 1 分 45 秒：

| 段 | LTR 忽略 | Wi-Fi 卡 L1 | 空闲根端口 |
|---|---|---|---|
| P0 | 不计 SATA（日常设置） | 原样（关） | 原样 |
| P1 | 不计 SATA | 开 | 原样 |
| P2 | 全部不计 | 开 | 原样 |
| P3 | 全部不计 | 开 | Link Disable |
| P4 | 全部不计 | 原样（关） | Link Disable |
| P5 | 不计 SATA | 原样（关） | 原样 |

每段记录的内容同 r5，另加：芯片组 xHCI 每个接着设备的 USB 口在采样中处于 U0、U2、U3 的次数（只读），以及这一段 Wi-Fi 卡和空闲根端口的实际状态。

## 它改什么

- LTR 忽略寄存器：同 r5。测完写回；ApplyMask 4 测完后保持，唤醒后 1 秒重设。
- Wi-Fi 卡（r6 新加）：只把卡的 Link Control 里的 L1 位（位 1）置 1，16 位写，其他位不动；根端口不写。下面全部满足才会写，否则只测不改，结果里写明原因：
  - 是 Intel 的无线网卡（class 02.80），挂在芯片组根端口下；
  - 卡在 D0，链路已建立；
  - 卡支持 L1，根端口已开 L1。
  - P4 起把卡的 ASPM 两位写回测试前的值。如果 AirportItlwm 在测试中途重启了网卡（它会再清掉 L1），那一段的结果里会写 “Wi-Fi L1 was cleared during the phase”。
- 空闲根端口（r6 新加）：P3、P4 把 Link Control 的 Link Disable（位 4）置 1，P5 起写回。只选下面没有设备、链路没起来、没被禁用的根端口，写之前再确认一次链路没起来。
- 测完、测试中途睡眠、驱动停止时都会写回；“done” 那一行会打印测试前后的值。
- 不改 macOS 设置、NVRAM、BIOS 和任何文件。重启后一切由 BIOS 和驱动重新设置。

风险（推断）：

- Wi-Fi 卡开 L1 是 Linux 和 Windows 下的常态，硬件自己进出 L1，对驱动透明。最坏情况是测试的约 50 秒里 Wi-Fi 短暂卡顿；测完恢复原样。
- 1c.0 下面没有设备，Link Disable 只是让它停止找设备。它是热插拔口，macOS 可能在日志里记一条插槽事件，没有别的影响。

r6 只做过语法检查，还没有在 macOS 上编译和运行。

## 编译（GitHub Actions）

仓库里换两个文件，workflow 不变：

| 仓库里的路径 | 内容 |
|---|---|
| `PkgCProbe/PkgCProbe.cpp` | 本目录的 `PkgCProbe.cpp`（r6） |
| `PkgCProbe/Info.plist` | 本目录的 `Info.plist`（r6：去掉 SataAlpm，新增 WifiL1 = false） |
| `.github/workflows/pkgcprobe.yml` | 本目录的 `pkgcprobe.yml`（没有改） |

在 GitHub 网页上打开文件，点铅笔图标，全选后粘贴新内容，提交。完成后下载 `PkgCProbe`，解压得到 `PkgCProbe.kext`。编译失败时下载 `PkgCProbe-build-log` 发我。

## 安装（OpenCore）

用新的 `PkgCProbe.kext` 整个替换 `EFI/OC/Kexts/PkgCProbe.kext`。config.plist 不用改。

## 测试（r6）

1. 插上电源重启，登录后不碰电脑约 9 分钟（屏幕亮着即可；Wi-Fi 保持连着，USB 设备保持平时的样子）。
2. 在终端运行（只读）：

```bash
ioreg -rc PkgCProbe -w0 > ~/Desktop/pkgc7.txt
```

3. 可选（约 1 分钟，测外接 USB Hub 的影响；步骤 2 之后再做，PkgCProbe 那时已测完）：

```bash
sudo -v
sudo powermetrics -i 5000 -n 3 --samplers cpu_power | grep "Package 0 C-state" > ~/Desktop/usb.txt
```

   拔下外接 USB Hub（键盘和接收器在上面），用笔记本自带的键盘，等 30 秒，再运行：

```bash
sudo powermetrics -i 5000 -n 3 --samplers cpu_power | grep "Package 0 C-state" >> ~/Desktop/usb.txt
```

   然后插回 Hub。powermetrics 只读；运行期间不要动触控板。

4. 把桌面上的 `pkgc7.txt`（和 `usb.txt`）发我。

## 怎么看结果

- P1 有 PC6 或更深：Wi-Fi 链路就是阻塞点，只开 Wi-Fi L1 就够。
- 只有 P2 有：Wi-Fi L1 之外还要不计 XHCI 的 LTR。
- 只有 P3 或 P4 有：和 1c.0 有关。
- 都没有：看 USB 计数和 usb.txt（拔 Hub 前后）。

## 选项

`Info.plist` → `IOKitPersonalities` → `PkgCProbe`：

| 键 | 本目录的值 | 说明 |
|---|---|---|
| StartDelay | 300 | 驱动启动后多少秒开始测试（30 到 3600） |
| PhaseSeconds | 15 | 每段测多少秒（5 到 120） |
| RunTest | true | 设为 false 则不测试；ApplyMask 开机即生效，WifiL1 在 StartDelay 秒后生效 |
| ApplyMask | 4 | 测完后一直不计的 LTR 位；4 = SATA，0 = 不保持 |
| WifiL1 | false | 设为 true：测完后（RunTest 为 false 时，在开机 StartDelay 秒后）打开 Wi-Fi 卡的 L1，之后每 30 秒检查一次，被 AirportItlwm 清掉就重新打开（计数在 ioreg 的 WifiL1Count）。先看 r6 的结果再决定，现在请保持 false |

## 停用

把 `config.plist` 里这一项的 `Enabled` 改为 False，重启。

## 修改记录

- r6（2026-11-18）：去掉 SATA ALPM（r5 证明盘不接受）。新增 Wi-Fi 卡 ASPM L1 和空闲根端口 Link Disable 的测试段；每段统计芯片组 USB 口的 U0/U2/U3 次数；只读概览多读空闲根端口的插槽寄存器和 Wi-Fi 链路详情；新增选项 WifiL1，去掉 SataAlpm；每段结果去掉总是相同的 bus delay。
- r5（2026-11-18）：去掉 PEG0 和 DMI 部分（已排除）。新增测试前的只读概览；测试改为 6 段，其中 4 段通过 AHCI 驱动开启 SATA ALPM（含 Slumber）。
- r4（2026-11-18）：只读 DMI 链路芯片组一侧；新增 3 段 PEG0 测试；每段多报 PC8 到 PC10。
- r3（2026-11-18）：只读 DMI 链路和 PEG0 的寄存器。Info.plist 的 ApplyMask 改为 4。
- r2（2026-11-17）：编译脚本改为按内容查找源文件。映射 PMC 寄存器改用 `map()`，查找 SATA 控制器改为在 PMC 所在的 PCI 总线上找。
- r1（2026-11-17）：第一版。

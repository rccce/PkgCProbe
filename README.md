# PkgCProbe：查 CPU 封装为什么停在浅层 C-state（诊断）

## 目前的结论（截至 r6）

- ApplyMask 4（不计 SATA 的 LTR）后能进 PC3，PC6 以上一直是 0。每次往深处走，都因为 I/O 总线（IOSF）没能停下而退回 PC3。
- r6（pkgc7）：
  - Wi-Fi 卡 L1 写入成功（卡的 LNKCTL 0x0142），测试中没被 AirportItlwm 清掉，PC6 仍是 0。
  - 所有 LTR 都不计，PC6 仍是 0。
  - 1c.0 Link Disable，PC6 仍是 0。
  - 芯片组 xHCI 上的 4 个 USB 设备，每一段 30 次采样全是 U0（工作中）。macOS 自己的端口统计（hackcheck 的 ioreg，port-statistics）也一样：HS02（外接 Hub）、HS04（指纹）、HS05（蓝牙）、HS06（摄像头）开机以来 On 99%，Suspended 0%。
  - usb.txt：拔掉外接 Hub 后，PC2 从约 34% 降到 12% 到 15%，PC3 从约 31% 升到 42% 到 57%，PC6 仍是 0。
- 推断：剩下的 3 个内置 USB 设备一直不挂起，xHCI 就一直不能空闲，封装进不了 PC6。
- 注意：之前排除的各项（DMI、PEG0、RTL8111、SATA ALPM、Wi-Fi L1、1c.0）都是在 USB 一直工作的情况下测的，只能说明“单改它一项不够”，不能说明它不是阻塞点之一。

## r7 做什么

用法和 r6 一样：驱动启动 StartDelay 秒（Info.plist 里是 300 秒）后开始，先只读一遍（同 r6），另加一行 PEG0（00:01.0）的状态。

然后分 6 段，每段先等 2 秒再测 15 秒，一共约 1 分 45 秒。每段在上一段的基础上多改一项：

| 段 | LTR 忽略 | Wi-Fi 卡 L1 | 空闲端口 Link Disable（1c.0、PEG0） | PEG0 通道和 bundle 断电 |
|---|---|---|---|---|
| P0 | 不计 SATA（日常设置） | 原样（关） | 原样 | 原样 |
| P1 | 不计 SATA | 开 | 原样 | 原样 |
| P2 | 全部不计 | 开 | 原样 | 原样 |
| P3 | 全部不计 | 开 | 开 | 原样 |
| P4 | 全部不计 | 开 | 开 | 开 |
| P5 | 不计 SATA | 原样（关） | 原样 | 原样 |

每段记录的内容同 r6。USB 计数多一列 other：接着设备、但既不在 U0、U2 也不在 U3 的次数（比如这个口不在 USB 映射里，macOS 没启用它）。

## 它改什么

- LTR 忽略寄存器、Wi-Fi 卡 L1、1c.0 的 Link Disable：同 r6。
- PEG0（r7 新加）：
  - P3、P4：Link Disable。只在 PEG0 是 8086:1901、下面没有设备、链路没起来时才写。
  - P4：PEG0 通道和 bundle 断电，按固件自己关独显的另一条路径（OEM SSDT 里的 PGOF，非 Windows 10 分支）的顺序：Link Disable 后最多等 300 毫秒让 LTSSM 变成 8，再置 0xC38 位 3、0xC20 位 5:4 = 3、8 个 bundle 寄存器（0x91C 到 0x9FC）的位 31。只在 L2/L3 握手已完成（0x248 位 14 = 1）时才写。
  - 为什么要测：config.plist 里的 `_OSI to XOSI` 加 SSDT-OC-XOSI 让固件以为系统是 “Windows 2015”，PGOF 因此走只做 L2/L3 的分支，PEG0 没有断电（r3 到 r5 读到 LNKCTL 0x0040、0xC20 = 0、0xC38 位 3 = 0）。r4 试过断电，但没先 Link Disable，和固件的顺序不同。
  - P5 起按固件 PGON 的顺序写回（0xC20、0xC38、bundle），然后才清 Link Disable。
- 测完、测试中途睡眠、驱动停止时都会写回；“done” 那一行会打印测试前后的值。
- 不改 macOS 设置、NVRAM、BIOS 和任何文件。重启后一切由 BIOS 和驱动重新设置。

风险（推断）：PEG0 下面的独显已经被 ACPI 断电，这些写入只影响这个空端口；r4 做过类似写入，没有问题。其余同 r6。

r7 只做过语法检查，还没有在 macOS 上编译和运行。

## 编译（GitHub Actions）

只换一个文件：

| 仓库里的路径 | 内容 |
|---|---|
| `PkgCProbe/PkgCProbe.cpp` | 本目录的 `PkgCProbe.cpp`（r7） |

`Info.plist` 和 `pkgcprobe.yml` 与 r6 相同，不用换。完成后下载 `PkgCProbe`，解压得到 `PkgCProbe.kext`。编译失败时下载 `PkgCProbe-build-log` 发我。

## 测试（这次开机临时停用 3 个内置 USB 设备）

准备（在 EFI 里）：

1. 备份 `EFI/OC/Kexts/USBPortsMap.kext/Contents/Info.plist`（比如复制到桌面）。
2. 用 `USBPortsMap-test/Info.plist` 替换它。它只从芯片组 XHC 的端口表里删掉 HS04（指纹）、HS05（蓝牙）、HS06（摄像头）三项，其他端口和 ASMedia 那张表都不变。这次开机这三个设备不会被启用，蓝牙和摄像头这次用不了。
3. 用 r7 的 `PkgCProbe.kext` 整个替换 `EFI/OC/Kexts/PkgCProbe.kext`。config.plist 不用改。
4. 拔掉所有 USB 设备（包括外接 Hub），这次用笔记本自带的键盘和触控板。

测试：

5. 插上电源重启，登录后马上在终端运行（它会等 PkgCProbe 测完，开机后约 7 分钟，再测亮屏 20 秒、关屏 20 秒；期间不要碰电脑）：

```bash
sudo bash ~/Downloads/pkgc2.sh
```

6. 显示 done 后运行：

```bash
mv ~/Desktop/pkgc2.txt ~/Desktop/pkgc8.txt
```

恢复：

7. 把第 1 步备份的 Info.plist 放回 `USBPortsMap.kext/Contents/`，插回 Hub，重启。蓝牙、摄像头恢复。r7 留着即可，日常仍只保持 ApplyMask 4。
8. 把 `pkgc8.txt` 发我。

## 怎么看结果

- P0 就有 PC6 或更深：USB 是主要阻塞点。下一步找出 3 个内置设备里是哪个挡住，以及怎样让它们空闲时挂起。
- P1 才有：还需要 Wi-Fi L1，这时 AirportItlwm 的补丁值得做。
- P2 才有：还需要不计某个 LTR。
- P3 才有：和 1c.0 或 PEG0 的 Link Disable 有关。
- P4 才有：PEG0 需要像固件的非 Windows 10 路径那样断电。
- 都没有：USB 不是唯一的阻塞点。pkgc2.sh 里关屏那段 pmtel 会显示 IOSF_NOT_BLOCKED 和 SB_NS 是否变少，再往下查 SATA 链路（盘不接受链路省电）、HD Audio、ME。

## 选项

`Info.plist` → `IOKitPersonalities` → `PkgCProbe`（与 r6 相同）：

| 键 | 本目录的值 | 说明 |
|---|---|---|
| StartDelay | 300 | 驱动启动后多少秒开始测试（30 到 3600） |
| PhaseSeconds | 15 | 每段测多少秒（5 到 120） |
| RunTest | true | 设为 false 则不测试；ApplyMask 开机即生效，WifiL1 在 StartDelay 秒后生效 |
| ApplyMask | 4 | 测完后一直不计的 LTR 位；4 = SATA，0 = 不保持 |
| WifiL1 | false | 设为 true：测完后打开 Wi-Fi 卡的 L1，每 30 秒检查一次，被 AirportItlwm 清掉就重新打开。r6 显示单开它不够，请保持 false |

## 停用

把 `config.plist` 里这一项的 `Enabled` 改为 False，重启。

## 修改记录

- r7（2026-11-18）：测试段改为逐段累加；空闲端口加入 PEG0（00:01.0）；新增 PEG0 通道和 bundle 断电（按固件 PGOF 的非 Windows 10 顺序，先 Link Disable）；USB 计数加 other 列。配合临时 USB 映射，在内置 USB 设备不工作时重测。
- r6（2026-11-18）：去掉 SATA ALPM（r5 证明盘不接受）。新增 Wi-Fi 卡 ASPM L1 和空闲根端口 Link Disable 的测试段；每段统计芯片组 USB 口的 U0/U2/U3 次数；只读概览多读空闲根端口的插槽寄存器和 Wi-Fi 链路详情；新增选项 WifiL1，去掉 SataAlpm；每段结果去掉总是相同的 bus delay。
- r5（2026-11-18）：去掉 PEG0 和 DMI 部分。新增测试前的只读概览；测试改为 6 段，其中 4 段通过 AHCI 驱动开启 SATA ALPM（含 Slumber）。
- r4（2026-11-18）：只读 DMI 链路芯片组一侧；新增 3 段 PEG0 测试；每段多报 PC8 到 PC10。
- r3（2026-11-18）：只读 DMI 链路和 PEG0 的寄存器。Info.plist 的 ApplyMask 改为 4。
- r2（2026-11-17）：编译脚本改为按内容查找源文件。映射 PMC 寄存器改用 `map()`，查找 SATA 控制器改为在 PMC 所在的 PCI 总线上找。
- r1（2026-11-17）：第一版。

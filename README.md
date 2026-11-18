# PkgCProbe r8：USB 空闲时给 PEG0 断电

## r7b（pkgc8）的结论

- 全程没有死机。r7b 做了 r7 除 PEG0 以外的所有步骤，所以 r7 的死机基本可以确定是 PEG0 的 Link Disable 引起的（推断）。
- 让 USB 完全空闲有用，但不够：
  - 芯片组 xHCI 的 24 个口都没有设备，XHCI 的 LTR 从 41 µs 变成“无要求”。
  - PC2 从约 39% 降到约 11%，PC3 从约 39% 升到 68% 到 75%。
  - 亮屏时，打断封装 C-state 的 SB_NS 事件从每秒约 3200 次降到约 1 次。
  - 推断：一直在工作的 USB 设备，是封装大量停在 PC2 的原因。
- PC6 到 PC10 仍然是 0：
  - 每次进 PC3 后封装想往深处走，都因为 IOSF_NOT_BLOCKED（CPU 的 I/O 总线没能停下）被降回 PC3。亮屏时每秒约 1700 次，和进 PC3 的次数几乎一样。
  - 全部 LTR 不计、1c.0 Link Disable、Wi-Fi L1 都没有改变这一点。
- 关屏那 20 秒不能用：至少有一个核在工作的时间约占 90%，封装 4.65 W。推断：关屏后系统开始跑积压的后台任务。

## 下一个嫌疑：PEG0（独显接口）

- 参考案例：一台 MacBookPro15,1（Coffee Lake）在 Linux 下也是封装停在 PC3，PC6 到 PC10 都是 0。作者试过把每个 PCH LTR 都设为不计、强制打开 ASPM，都没有用。最后按 macOS 的方式把 PEG0 停好（L2/L3 握手、通道断电、总线号 0x00FEFF00），才进到 PC7 到 PC10。来源：https://gist.github.com/axxapy/d0d177d3adfef4822910bc9521f89cce
- 你的 PEG0：
  - L2/L3 握手已完成（0x248 = 0x00004000，LTSSM 0x165），和那台机器停好后的值一样。
  - 但通道和 bundle 没断电（0xC20 = 0，0xC38 位 3 = 0），总线号仍是 0x00010100。
  - 原因：config.plist 的 `_OSI to XOSI` 让固件以为系统是 Windows 10，关独显时只走了做 L2/L3 的那条路。
- r4 试过通道和 bundle 断电，PC6 仍是 0，但当时 USB 一直在工作。“USB 空闲 + PEG0 断电”从没一起测过。
- 反面证据：那位作者说起作用的是握手，事后手动置断电位没用；而你的握手已经完成。所以 PEG0 也可能不是原因（推断），r8 正好能确认或排除。

## r8 做什么

以 r7b 为基础。驱动启动 300 秒后开始，分 8 段。每段先在终端预告 7 秒，再改设置，等 2 秒后测 15 秒，一共约 3 分钟。P1 到 P5 每段在上一段的基础上多改一项：

| 段 | LTR 忽略 | PEG0 | 1c.0 Link Disable、Wi-Fi L1 |
|---|---|---|---|
| P0 | 不计 SATA（日常设置） | 原样 | 原样 |
| P1 | 全部不计 | 原样 | 原样 |
| P2 | 全部不计 | 通道和 bundle 断电 | 原样 |
| P3 | 全部不计 | + 总线号 0x00FEFF00 | 原样 |
| P4 | 全部不计 | + D3hot | 原样 |
| P5 | 全部不计 | 同 P4 | 开 |
| P6 | 不计 SATA | 断电 + 总线号（候选日常设置） | 原样 |
| P7 | 不计 SATA | 原样 | 原样 |

测完后保持 90 秒：只不计 SATA，PEG0 断电加总线号，让 pkgc2.sh 的亮屏、关屏两次 pmtel 测这个状态，然后全部写回。

安全措施：

- 通道、bundle、D3hot 写的值和 r4 完全一样，r4 跑过两次没出事。总线号 0x00FEFF00 是新写入，按参考案例里 Apple 固件的做法。
- 从不写 Link Disable。
- 只在 PEG0 停好时才写：8086:1901、下面没有设备、链路没起来也没被禁用、L2/L3 握手已完成。
- 测完、测试或保持期间睡眠、关机、重启、驱动停止，都会先写回。推断：固件在睡眠和关机前会给独显上电，PEG0 若还断着电，可能卡住。
- 收不到关机、重启通知时完全不碰 PEG0。

推断：风险低，但 r4 每次只断电约 17 秒，r8 在测试中会断电约 2 分钟，测完后再断电 1 分半。万一死机，终端最后一行就是出事的那一段。

r8 只做过语法检查和代码审查，还没有在 macOS 上编译和运行。

## 编译（GitHub Actions）

只换一个文件：

| 仓库里的路径 | 内容 |
|---|---|
| `PkgCProbe/PkgCProbe.cpp` | 本目录的 `PkgCProbe.cpp`（r8） |

`Info.plist` 和 `pkgcprobe.yml` 不用换。完成后下载 `PkgCProbe`，解压得到 `PkgCProbe.kext`。编译失败时下载 `PkgCProbe-build-log` 发我。

## 测试（和 r7b 一样）

准备：

1. 用 r8 的 `PkgCProbe.kext` 整个替换 `EFI/OC/Kexts/PkgCProbe.kext`。确认它的 `Contents/Info.plist` 里 `RunTest` 是 `<true/>`（新编译的默认就是）。
2. 用新的 `pkgc2.sh` 覆盖 `~/Downloads/pkgc2.sh`。
3. 用 `USBPortsMap-test/Info.plist` 替换 `EFI/OC/Kexts/USBPortsMap.kext/Contents/Info.plist`。
4. 拔掉所有 USB 设备（包括外接 Hub）。

测试：

5. 插上电源重启，登录后马上在终端运行，然后不要碰电脑，约开机后 10 分钟显示 done：

```bash
sudo bash ~/Downloads/pkgc2.sh
```

6. 显示 done 后运行：

```bash
mv ~/Desktop/pkgc2.txt ~/Desktop/pkgc9.txt
```

万一死机：先用手机拍下终端窗口，再长按电源键关机。

恢复：

7. 把 `USBPortsMap.kext/Contents/Info.plist` 换回原版；把 `PkgCProbe.kext/Contents/Info.plist` 里 `RunTest` 改回 `<false/>`；插回 Hub，重启。
8. 把 `pkgc9.txt`（或照片）发我。

## 怎么看结果

- P2 起有 PC6 或更深：PEG0 通道断电就是关键。下一步做日常修复：驱动在开机和每次唤醒后给 PEG0 断电，睡眠、关机、重启前写回。
- P3 才有：还需要总线号。P4 才有：还需要 D3hot。P5 才有：还需要 1c.0 或 Wi-Fi L1。
- P6（只不计 SATA）比 P3 浅：ME 的 LTR（131.1 µs）挡住了更深的状态（PC7 需要 151.6 µs），日常设置还要不计 ME。
- 都没有：PEG0 不是（或不是唯一的）原因。看保持期间 pmtel 的 IOSF_NOT_BLOCKED 有没有变少。下一步考虑用 SSDT 让固件走它自己的非 Windows 10 关独显流程，或查显示和 DMI。

## 选项

`Info.plist` → `IOKitPersonalities` → `PkgCProbe`：

| 键 | 本目录的值 | 说明 |
|---|---|---|
| StartDelay | 300 | 驱动启动后多少秒开始测试（30 到 3600） |
| PhaseSeconds | 15 | 每段测多少秒（5 到 120） |
| RunTest | true | 设为 false 则不测试；ApplyMask 开机即生效，WifiL1 在 StartDelay 秒后生效 |
| ApplyMask | 4 | 测完后一直不计的 LTR 位；4 = SATA，0 = 不保持 |
| WifiL1 | false | 设为 true：测完后打开 Wi-Fi 卡的 L1，每 30 秒检查一次。请保持 false |
| PegHoldSeconds | （没有这一项） | r8：测完后 PEG0 保持断电的秒数，默认 90，0 = 不保持，最多 600 |

## 停用

把 `config.plist` 里这一项的 `Enabled` 改为 False，重启。

## 修改记录

- r8（2026-11-18）：以 r7b 为基础，新增 PEG0 通道和 bundle 断电、D3hot（与 r4 相同）和总线号 0x00FEFF00，从不写 Link Disable；测试段改为 8 段；测完后保持 PEG0 断电 90 秒供 pmtel 测量；睡眠、关机、重启前写回 PEG0。pkgc2.sh 等保持结束后再收尾。
- r7b（2026-11-18）：以 r6 为基础，不写 PEG0；测试段逐段累加，Wi-Fi L1 放最后；新增 ioreg 属性 Phase；xHCI、HD Audio、RTL8111 只在 D0 时读寄存器；USB 计数加 other 列。pkgc2.sh 在屏幕上显示进度。
- r7（2026-11-18）：新增 PEG0 Link Disable 和断电。配合测试版 USB 映射，每次都在测试中硬死机（推断由 PEG0 Link Disable 引起），已废弃。
- r6（2026-11-18）：去掉 SATA ALPM。新增 Wi-Fi 卡 ASPM L1 和空闲根端口 Link Disable 的测试段；每段统计芯片组 USB 口的 U0/U2/U3 次数；新增选项 WifiL1。
- r5（2026-11-18）：去掉 PEG0 和 DMI 部分。新增测试前的只读概览；4 段通过 AHCI 驱动开启 SATA ALPM。
- r4（2026-11-18）：只读 DMI 链路芯片组一侧；新增 3 段 PEG0 测试（D3hot、通道和 bundle 断电，无 Link Disable）；每段多报 PC8 到 PC10。
- r3（2026-11-18）：只读 DMI 链路和 PEG0 的寄存器。Info.plist 的 ApplyMask 改为 4。
- r2（2026-11-17）：编译脚本改为按内容查找源文件；映射 PMC 寄存器改用 `map()`。
- r1（2026-11-17）：第一版。

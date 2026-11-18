# PkgCProbe：查 CPU 封装为什么停在浅层 C-state（诊断）

## 目前的结论

- SATA 控制器一直上报 10.2 µs 的延迟要求（LTR），把封装限制在 PC2。ApplyMask 4（不计 SATA 的 LTR）后能进 PC3，插电、电池、唤醒后都已验证。
- PC6 以上一直是 0。每次往深处走，都因为 I/O 总线（IOSF）没能停下而退回 PC3。
- 已排除（r4，pkgc4 和 pkgc5）：DMI 链路两端的 ASPM L1、独显口 PEG0（D3hot、通道断电、束断电都试过）、RTL8111 链路的 ASPM L1（`pci-aspm-default = 2`）。芯片组所有 LTR 都不计时，也没有 PC6。
- 新线索：所有测试里 SATA 链路每次采样都是 active，PxCMD.ALPE 是 0（链路省电从没开过）。Intel 从 Haswell 起，SATA 链路保持全功率就会让芯片组一直“在忙”，封装进不了更深的状态（见下方出处）。
- 你 EFI 里的 AHCI 驱动 CtlnaAHCIPort 341.0.2 带有 Apple 自己的 ALPM 代码：开启后，控制器在链路空闲时自动进 Partial；驱动在 80 ms 没有读写后再把链路转到 Slumber。但本机上从没有被开启（推断：开启它的是 IOAHCIBlockStorage，条件在本机不满足）。r5 用来验证这条线索。

出处：Linux 内核补丁 “libata: Add new med_power_with_dipm link_power_management_policy setting”（Hans de Goede，2017）；Matthew Garrett，“Reducing power consumption on Haswell and Broadwell systems”（2015）。

## 它做什么

驱动启动 StartDelay 秒后开始（本目录的 Info.plist 是 300 秒，即 5 分钟）。

先只读一遍，每项一行：

- 芯片组各模块的断电状态（PPFEAR）：哪些开着，哪些已断电。
- 每个芯片组 PCIe 根端口和后面的设备：ASPM、时钟电源管理、LTR 开关、L1 子状态。
- 每个 xHCI 控制器：哪些 USB 口接了设备、速度、链路状态（U0 = 工作中，U2、U3 = 休眠）。
- HD Audio：有没有正在运行的音频数据流。
- RTL8111 芯片自己的 ASPM 开关（Config5 位 0）、时钟请求开关（Config2 位 7），以及网线是否连着。
- AHCI 端口的更多寄存器（PxIE、PxIS、PxSERR、PxCI、PxSACT）。

然后分 6 段测量，每段先等 2 秒再测 15 秒，一共约 1 分 45 秒：

| 段 | LTR 忽略 | SATA ALPM |
|---|---|---|
| P0 | 不计 SATA（日常设置） | 原样（关） |
| P1 | 不计 SATA | 开（链路空闲时由控制器转入 Partial） |
| P2 | 不计 SATA | 开，并请求一次 Slumber |
| P3 | 全部计入 | 开 + Slumber（看开 ALPM 后还要不要 ApplyMask） |
| P4 | 全部不计 | 开 + Slumber |
| P5 | 不计 SATA | 恢复原样（关） |

每段记录的内容和 r4 相同，另加：几个芯片组模块（OPI-DMI、XHCI、SPA、SPB、SPC、SATA、HDA）在采样中处于断电状态的次数，SATA ALPM 的状态和 Slumber 请求的结果。

## 它改什么

- LTR 忽略寄存器：同 r4。测完写回；ApplyMask 4 测完后保持，唤醒后 1 秒重设。
- SATA ALPM（r5 新加）：P1 到 P4 约 70 秒里开着，P5 起恢复原样。开和关都是调用 AHCI 驱动自己的 Enable/Disable 函数，并在驱动自己的工作队列里执行，不会和驱动的读写同时进行。Slumber 按驱动计时器的同样方法请求（PxCMD.ICC），只在没有未完成的读写命令时请求。关掉后会唤醒链路，并清掉链路切换留在 PxSERR 里的两个状态位（PhyRdy 变化、Comm Wake），其他位不动。测试中途睡眠或驱动停止时，也会先恢复。
- 下面几条全部满足才会开 ALPM，任何一条不满足就只测不改，结果里写明原因：
  - 驱动对象是 CtlnaIntelPchSeriesAHCIPort；
  - 驱动的 4 个 ALPM 函数和分析过的 341.0.2 逐字节相同；
  - 控制器支持 ALPM、Partial 和 Slumber；
  - 端口在运行、接着盘；
  - 端口没有开 PhyRdy 变化中断（不是热插拔口）。这种端口上，驱动对链路切换不做处理，不会当成拔盘。
- 不改 macOS 设置、NVRAM、BIOS 和任何文件。重启后一切由 BIOS 和驱动重新设置。

风险（推断）：

- ALPM 开着时，SATA 盘空闲后的第一次读写最多多等约 10 ms（从 Slumber 唤醒）。
- 个别 SSD 的固件处理链路省电有缺陷。Linux 上 min_power 策略有过死机，甚至数据损坏的报告；Windows 和 Apple 用的“控制器进 Partial、软件再请求 Slumber”一般认为是安全的。测试只有约 70 秒，而且在空闲时。最坏情况是这块盘读写出错或被弹出，重启后恢复原样（平时 ALPM 是关的）。测试时请不要往 Dev 盘写东西；重要数据平时也应有备份。

r5 只做过语法检查，还没有在 macOS 上编译和运行。

## 编译（GitHub Actions）

仓库里换两个文件，workflow 不变：

| 仓库里的路径 | 内容 |
|---|---|
| `PkgCProbe/PkgCProbe.cpp` | 本目录的 `PkgCProbe.cpp`（r5） |
| `PkgCProbe/Info.plist` | 本目录的 `Info.plist`（r5：新增 SataAlpm = false，StartDelay 改为 300） |
| `.github/workflows/pkgcprobe.yml` | 本目录的 `pkgcprobe.yml`（没有改） |

在 GitHub 网页上打开文件，点铅笔图标，全选后粘贴新内容，提交。完成后下载 `PkgCProbe`，解压得到 `PkgCProbe.kext`。编译失败时下载 `PkgCProbe-build-log` 发我。

## 安装（OpenCore）

- 用新的 `PkgCProbe.kext` 整个替换 `EFI/OC/Kexts/PkgCProbe.kext`。
- RealtekRTL8111 的 `disableASPM` 保持现在的 false（r5 会读出芯片自己的 ASPM 开关）。
- ADJP 的补丁见 `ACPI-ADJP/README.md`，可以和这次一起重启。

## 测试（r5）

1. 插上电源重启，登录后不碰电脑约 9 分钟（屏幕亮着即可；USB 设备和网络保持平时的样子）。
2. 在终端运行（只读）：

```bash
ioreg -rc PkgCProbe -w0 > ~/Desktop/pkgc6.txt
```

3. 把桌面上的 `pkgc6.txt` 发我。

## 选项

`Info.plist` → `IOKitPersonalities` → `PkgCProbe`：

| 键 | 本目录的值 | 说明 |
|---|---|---|
| StartDelay | 300 | 驱动启动后多少秒开始测试（30 到 3600） |
| PhaseSeconds | 15 | 每段测多少秒（5 到 120） |
| RunTest | true | 设为 false 则不测试；ApplyMask 开机即生效，SataAlpm 在 StartDelay 秒后生效 |
| ApplyMask | 4 | 测完后一直不计的 LTR 位；4 = SATA，0 = 不保持 |
| SataAlpm | false | 设为 true：测完后（RunTest 为 false 时，在开机 StartDelay 秒后）通过驱动一直开着 SATA ALPM，每次唤醒 5 秒后再开一次。先看 r5 的结果再决定，现在请保持 false |

## 停用

把 `config.plist` 里这一项的 `Enabled` 改为 False，重启。

## 修改记录

- r5（2026-11-18）：去掉 PEG0 和 DMI 部分（已排除）。新增测试前的只读概览：芯片组模块断电状态（PPFEAR）、PCIe 根端口和设备、xHCI 端口、HD Audio 数据流、RTL8111 芯片自己的 ASPM 开关、更多 AHCI 寄存器。测试改为 6 段，其中 4 段通过 AHCI 驱动开启 SATA ALPM（含 Slumber）。新增选项 SataAlpm。每段多报芯片组模块的断电次数。
- r4（2026-11-18）：只读 DMI 链路芯片组一侧；新增 3 段 PEG0 测试（D3hot、通道断电、束断电，每段后写回）；每段多报 PC8 到 PC10。
- r3（2026-11-18）：只读 DMI 链路和 PEG0 的寄存器。Info.plist 的 ApplyMask 改为 4。
- r2（2026-11-17）：编译脚本改为按内容查找源文件。映射 PMC 寄存器改用 `map()`，查找 SATA 控制器改为在 PMC 所在的 PCI 总线上找。
- r1（2026-11-17）：第一版。

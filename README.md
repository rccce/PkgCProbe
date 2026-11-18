# PkgCProbe：查 CPU 封装为什么停在浅层 C-state（诊断）

## 目前的结论

- SATA 控制器一直上报 10.2 µs 的延迟要求（LTR），把封装限制在 PC2。让芯片组不计 SATA 的 LTR（ApplyMask 4）后，封装能进 PC3。插电和电池下结果相同。
- 即使不计所有芯片组 LTR、屏幕也关着，PC6、PC7 仍是 0。还有一个不是 LTR 的阻碍，r3 用来查它是否在 CPU 的 PCIe 显卡口（PEG0）或 DMI 链路上。

## 它做什么

开机约 3 分钟后开始，分 6 段测量，每段 15 秒，一共约 2 分钟：

| 段 | 内容 |
|---|---|
| P0 | 不改动，测基线 |
| P1 | 让芯片组暂时不计 SATA 的 LTR |
| P2 | 暂时不计 XHCI 的 LTR |
| P3 | SATA 和 XHCI 都不计 |
| P4 | 所有芯片组设备的 LTR 都不计 |
| P5 | 写回原值，再测一次基线 |

每段记录封装 PC2、PC3、PC6、PC7 的驻留比例、CPU 忙碌比例、封装和内存功耗、各设备的 LTR、SATA 链路状态、封装 C-state 上限和 macOS 的总线延迟限制。

r3 另外只读三组寄存器，在开机时、测完后、前三次唤醒后各读一次：

- 主桥 00:00.0：哪些 CPU 侧设备开着（DEVEN），DMI 链路（CPU 到芯片组）的 ASPM 设置和状态。
- PEG0（00:01.0，独显所在的 CPU PCIe 口）：命令、总线号、电源状态、链路和插槽寄存器。
- PEG0 的扩展寄存器 0x248、0xC20、0xC38、0x91C 到 0x97C、0xC74。公开资料显示，在同代 CPU 的 MacBook Pro 上，独显断电前固件会通过这些寄存器让 PEG0 进入 L2/L3 并关闭通道；不这样做时封装停在 PC3。这里只读，看你的机器上 PEG0 是什么状态。

## 它改什么

只改芯片组电源管理控制器（PMC）里的 LTR 忽略寄存器：

- 测试的约 2 分钟里按上表改，测完写回原值；测试中途睡眠也会先写回。
- ApplyMask 不为 0 时，测完后保持这些位，每次唤醒 1 秒后再设一次。本目录的 Info.plist 里 ApplyMask 是 4（只不计 SATA）。

r3 新加的部分只读，不写任何寄存器。不改 macOS 设置、NVRAM、BIOS 和任何文件。重启后 LTR 忽略寄存器由 BIOS 重新设置。

风险（推断）：不计 SATA 的 LTR 后，这块 SATA 盘在空闲后第一次读写可能多等几十微秒，数据不会出错。

r2 已在你的机器上编译并运行过；r3 新加的部分只做过语法检查，还没有在 macOS 上编译和运行。

## 编译（GitHub Actions）

仓库里要有这三个文件：

| 仓库里的路径 | 内容 |
|---|---|
| `PkgCProbe/PkgCProbe.cpp` | 本目录的 `PkgCProbe.cpp` |
| `PkgCProbe/Info.plist` | 本目录的 `Info.plist` |
| `.github/workflows/pkgcprobe.yml` | 本目录的 `pkgcprobe.yml`（r3 没有改） |

在 GitHub 网页上打开文件，点铅笔图标，全选后粘贴新内容，提交。每提交一个文件，Actions 就会跑一次，用最后一次提交对应的那次运行。完成后下载 `PkgCProbe`，解压得到 `PkgCProbe.kext`。编译失败时下载 `PkgCProbe-build-log` 发我。

## 安装（OpenCore）

用新的 `PkgCProbe.kext` 整个替换 `EFI/OC/Kexts/PkgCProbe.kext`。config.plist 不用改（Kernel → Add 里已有这一项）。

## 测试（r3）

1. 重启，登录后不碰电脑约 6 分钟（插电或电池都可以）。
2. 在终端运行：

```bash
ioreg -rc PkgCProbe -w0 > ~/Desktop/pkgc3.txt
```

3. 之后正常用。下一次合盖睡眠、唤醒后，再运行一次：

```bash
ioreg -rc PkgCProbe -w0 > ~/Desktop/pkgc3-wake.txt
```

4. 把桌面上这两个文件发我。两条命令都只读。

## 选项

`Info.plist` → `IOKitPersonalities` → `PkgCProbe`：

| 键 | 本目录的值 | 说明 |
|---|---|---|
| StartDelay | 180 | 驱动启动后多少秒开始测试（30 到 3600） |
| PhaseSeconds | 15 | 每段测多少秒（5 到 120） |
| RunTest | true | 设为 false 则不测试（ApplyMask 在开机时直接生效） |
| ApplyMask | 4 | 测完后一直不计的 LTR 位；4 = SATA，0 = 不保持 |

## 停用

把 `config.plist` 里这一项的 `Enabled` 改为 False，重启。

## 修改记录

- r3（2026-11-18）：只读 DMI 链路和 PEG0 的寄存器，在开机时、测完后、前三次唤醒后各读一次。测试内容和写入的寄存器不变。Info.plist 的 ApplyMask 改为 4。
- r2（2026-11-17）：编译脚本改为按内容查找源文件，并在日志里列出仓库文件。映射 PMC 寄存器改用 `map()`，查找 SATA 控制器改为在 PMC 所在的 PCI 总线上找。
- r1（2026-11-17）：第一版。

# PkgCProbe：查 CPU 封装为什么停在浅层 C-state（诊断）

## 目前的结论

- SATA 控制器一直上报 10.2 µs 的延迟要求（LTR），把封装限制在 PC2。让芯片组不计 SATA 的 LTR（ApplyMask 4）后，封装能进 PC3。插电和电池下结果相同，唤醒后会自动重设（r3 已验证）。
- 即使不计所有芯片组 LTR、屏幕也关着，PC6、PC7 仍是 0。CPU 每次想往 PC6 走，都因为 CPU 侧的 I/O 总线（IOSF）没能停下而退回 PC3。
- r3 读到：独显口 PEG0 已经做完断电前的 L2/L3 握手（0x248 = 0x4000，0xC74 = 0x165，和 MacBook Pro 上停好的状态相同），但没有做通道断电，口本身还在 D0；DMI 链路的 CPU 这一侧开着 ASPM L1。
- 固件的 PegSsdt 里，Windows 10 走的是“只握手”这条路（你的 OpenCore 让 macOS 也走这条路），其他系统走“关链路 + 通道断电 + 束断电”；Windows 下系统通常还会把 PEG0 放进 D3（推断）。r4 用来逐项试这些差别，并读出 DMI 芯片组那一侧的 ASPM 设置。

## 它做什么

开机约 3 分钟后开始，分 7 段测量，每段先等 2 秒再测 15 秒，一共约 2 分钟：

| 段 | 内容 |
|---|---|
| P0 | 不改动，测基线 |
| P1 | 让芯片组暂时不计 SATA 的 LTR |
| P2 | 所有芯片组设备的 LTR 都不计 |
| P3 | 同 P2，另把 PEG0 放进 D3hot |
| P4 | 同 P2，另关 PEG0 的通道电源（0xC20 位 5:4 = 3，0xC38 位 3 = 1） |
| P5 | 同 P2，另 D3hot + 关通道电源 + 关 8 个束（0x91C 到 0x9FC 的位 31） |
| P6 | 全部写回原值，再测一次基线 |

每段记录封装 PC2、PC3、PC6、PC7、PC8、PC9、PC10 的驻留比例、CPU 忙碌比例、封装和内存功耗、各设备的 LTR、SATA 链路状态、封装 C-state 上限和 macOS 的总线延迟限制。P3 到 P5 还记录 PEG0 改成了什么、是否已写回。

只读的部分，在开机时、测完后、前三次唤醒后各读一次：

- 主桥 00:00.0：哪些 CPU 侧设备开着（DEVEN），DMI 链路 CPU 这一侧的 ASPM 设置和状态。
- r4 新加：DMI 链路芯片组那一侧的 ASPM 设置和状态，经芯片组的边带寄存器窗口（0xFD000000，端口 0xEF）读取。同一窗口里的 TCO 基址要和 SMBus 控制器报的一致，结果才可信，结果行里会写 match 或 NO MATCH。
- PEG0（00:01.0）：命令、总线号、电源状态、链路和插槽寄存器，以及扩展寄存器 0x248、0xC20、0xC38、0x91C 到 0x9FC、0xC74。

## 它改什么

- 芯片组电源管理控制器（PMC）里的 LTR 忽略寄存器：测试的约 2 分钟里按上表改，测完写回原值。ApplyMask 不为 0 时，测完后保持这些位，每次唤醒 1 秒后再设一次。本目录的 Info.plist 里 ApplyMask 是 4（只不计 SATA）。
- r4 新加：P3 到 P5 时临时改 PEG0 的电源状态、通道和束的电源位，每段测完立刻写回原值并核对。测试中途睡眠或驱动停止时也会先写回。只有满足全部条件才会写：PEG0 是 8086:1901，后面没有任何设备，链路是断开的，L2/L3 握手已完成，PMC 寄存器也正常。任一条不满足就只测不写，结果里写明原因。
- 不改 macOS 设置、NVRAM、BIOS 和任何文件。重启后这些寄存器都由 BIOS 重新设置。

风险（推断）：

- 不计 SATA 的 LTR 后，这块 SATA 盘在空闲后第一次读写可能多等几十微秒，数据不会出错。
- P2 到 P5 不计所有 LTR，如果 P3 到 P5 里封装真的进了更深的 C-state，这几十秒内 USB 音频之类可能偶尔卡一下。
- PEG0 的这些位只影响已经断电的独显口。万一写回不完整，最坏是下一次睡眠前独显上电时链路起不来，macOS 本来就不用独显，重启即恢复。

r3 已在你的机器上编译并运行过；r4 新加的部分只做过语法检查，还没有在 macOS 上编译和运行。

## 编译（GitHub Actions）

仓库里要有这三个文件：

| 仓库里的路径 | 内容 |
|---|---|
| `PkgCProbe/PkgCProbe.cpp` | 本目录的 `PkgCProbe.cpp`（r4 只改了这个文件） |
| `PkgCProbe/Info.plist` | 本目录的 `Info.plist`（和 r3 相同，不用换） |
| `.github/workflows/pkgcprobe.yml` | 本目录的 `pkgcprobe.yml`（没有改） |

在 GitHub 网页上打开文件，点铅笔图标，全选后粘贴新内容，提交。完成后下载 `PkgCProbe`，解压得到 `PkgCProbe.kext`。编译失败时下载 `PkgCProbe-build-log` 发我。

## 安装（OpenCore）

用新的 `PkgCProbe.kext` 整个替换 `EFI/OC/Kexts/PkgCProbe.kext`。config.plist 不用改。

## 测试（r4）

1. 重启，登录后不碰电脑约 8 分钟（插电或电池都可以，屏幕亮着即可）。
2. 在终端运行（只读）：

```bash
ioreg -rc PkgCProbe -w0 > ~/Desktop/pkgc4.txt
```

3. 把桌面上的 `pkgc4.txt` 发我。

## 选项

`Info.plist` → `IOKitPersonalities` → `PkgCProbe`：

| 键 | 本目录的值 | 说明 |
|---|---|---|
| StartDelay | 180 | 驱动启动后多少秒开始测试（30 到 3600） |
| PhaseSeconds | 15 | 每段测多少秒（5 到 120） |
| RunTest | true | 设为 false 则不测试（ApplyMask 在开机时直接生效，也不会改 PEG0） |
| ApplyMask | 4 | 测完后一直不计的 LTR 位；4 = SATA，0 = 不保持 |

## 停用

把 `config.plist` 里这一项的 `Enabled` 改为 False，重启。

## 修改记录

- r4（2026-11-18）：只读 DMI 链路芯片组一侧；新增 3 段 PEG0 测试（D3hot、通道断电、束断电，每段后写回）；去掉“只不计 XHCI”“SATA+XHCI”两段；每段多报 PC8 到 PC10；PEG0 的束寄存器读全 8 个。
- r3（2026-11-18）：只读 DMI 链路和 PEG0 的寄存器，在开机时、测完后、前三次唤醒后各读一次。Info.plist 的 ApplyMask 改为 4。
- r2（2026-11-17）：编译脚本改为按内容查找源文件，并在日志里列出仓库文件。映射 PMC 寄存器改用 `map()`，查找 SATA 控制器改为在 PMC 所在的 PCI 总线上找。
- r1（2026-11-17）：第一版。

# PkgCProbe：验证 CPU 封装为什么停在 PC2（一次性诊断）

pmtel 的结果显示，芯片组里 SATA 控制器一直上报 10.2 µs 的延迟要求（LTR），XHCI 上报 41 µs。PkgCProbe 用来验证是不是它们拦住了 CPU 封装。

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

每段记录：

- 封装 PC2、PC3、PC6、PC7 的驻留比例，CPU 忙碌比例
- 封装功耗、内存功耗
- 芯片组当前生效的 LTR，以及 SATA、XHCI、ME、HDA 各自的 LTR
- SATA 链路处在 active、partial 还是 slumber
- CPU 的封装 C-state 上限（MSR 0xE2）
- macOS 当时的总线延迟限制（播放声音时 AppleHDAController 会设成 15 µs）

开机时还会读一次 PC3、PC6、PC7 的延迟门槛（MSR 0x60A 到 0x60C）和 SATA 端口的电源管理设置。

## 它改什么

只改芯片组电源管理控制器（PMC）里的一个寄存器：LTR 忽略寄存器。只在测试的约 2 分钟里改，测完写回原值；测试中途睡眠也会先写回。重启后这个寄存器由 BIOS 重新设置。

不改 macOS 设置、NVRAM、BIOS 和任何文件。寄存器内容和预期不符时，它只读不写，并在结果里注明。

风险（推断）：被“不计”的设备在那 15 秒里访问内存可能要多等几十到一百多微秒。最坏的情况是 USB 或声音短暂卡一下，数据不会出错。测试时不要放声音，也不要拷贝大文件。

这个 kext 还没在 macOS 上编译和运行过，只做过语法检查。

## 编译（GitHub Actions）

仓库里要有这三个文件：

| 仓库里的路径 | 内容 |
|---|---|
| `PkgCProbe/PkgCProbe.cpp` | 本目录的 `PkgCProbe.cpp` |
| `PkgCProbe/Info.plist` | 本目录的 `Info.plist` |
| `.github/workflows/pkgcprobe.yml` | 本目录的 `pkgcprobe.yml` |

在 GitHub 网页上建带文件夹的文件：Add file → Create new file，文件名一栏输入 `PkgCProbe/PkgCProbe.cpp`（输入 `/` 时自动变成文件夹），粘贴内容，提交。另外两个文件同样处理。

编译脚本按文件内容查找这两个源文件，放在别的文件夹也能编。仓库根目录如果有 InitHeadphone 的 `Info.plist`，不要把这里的 `Info.plist` 传到根目录，否则会替换掉它。

每提交一个文件，Actions 就会跑一次。用最后一次提交对应的那次运行；也可以在 Actions → `PkgCProbe build` → Run workflow 手动再跑一次。完成后下载 `PkgCProbe`，解压得到 `PkgCProbe.kext`。编译失败时下载 `PkgCProbe-build-log` 发我，日志开头会列出仓库里的文件。

## 安装（OpenCore）

1. 把 `PkgCProbe.kext` 放进 `EFI/OC/Kexts/`。
2. 在 `config.plist` 的 `Kernel → Add` 里加一项。它不依赖 Lilu，位置随意，放最后即可：

| 键 | 值 |
|---|---|
| Arch | Any |
| BundlePath | PkgCProbe.kext |
| Comment | package C-state probe |
| Enabled | True |
| ExecutablePath | Contents/MacOS/PkgCProbe |
| MaxKernel | （空） |
| MinKernel | （空） |
| PlistPath | Contents/Info.plist |

3. 重启。

## 测试

1. 开机登录后，屏幕保持亮着，不碰电脑，不放声音，等 6 分钟。电源状态和上次跑 pmtel 时一样即可（上次接着电源）。
2. 在终端运行：

```bash
ioreg -rc PkgCProbe -w0 > ~/Desktop/pkgc.txt
log show --last 30m --style compact --predicate 'eventMessage CONTAINS "PkgCProbe"' >> ~/Desktop/pkgc.txt
pmset -g batt | head -n 2 >> ~/Desktop/pkgc.txt
```

3. 把桌面上的 `pkgc.txt` 发我。

`ioreg` 输出里 `"State" = "done"` 表示已经测完；`waiting` 或 `running` 表示还没测完，过一两分钟再运行一次。三条命令都只读。

## 选项

`Info.plist` → `IOKitPersonalities` → `PkgCProbe`：

| 键 | 默认 | 说明 |
|---|---|---|
| StartDelay | 180 | 驱动启动后多少秒开始测试（30 到 3600） |
| PhaseSeconds | 15 | 每段测多少秒（5 到 120） |
| RunTest | true | 设为 false 则不测试 |
| ApplyMask | 0 | 先不要改。看完结果再决定要不要用 |

## 停用

把 `config.plist` 里这一项的 `Enabled` 改为 False，重启。

## 修改记录

- r2（2026-11-17）：编译脚本改为按内容查找源文件，并在日志里列出仓库文件。代码里映射 PMC 寄存器改用 `map()`，查找 SATA 控制器改为在 PMC 所在的 PCI 总线上找。测试内容、写入的寄存器和范围都不变。
- r1（2026-11-17）：第一版。

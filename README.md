# X2600M / HALLEY7-M FreeRTOS ReGBA

本仓库保存 X2600M 主核 FreeRTOS 的 ReGBA 移植源码、构建脚本、主机测试和分阶段实施记录。当前使用入口为[掌机工程说明](projects/halley7m-regba/README.md)，NOR 安装和更新方式见[烧录步骤](docs/烧录步骤.md)。提交按现有最终代码的功能边界分组导入，集成提交之前的中间提交不保证独立构建，不代表开发当时的历史快照。下文按当前功能和早期环境记录保留各版验收范围，不应将历史版本的测试结论合并为当前版本结果。

后续每完成一个阶段或独立功能，在完成相关验证后使用中文标题和正文单独提交，并及时推送至本仓库。具体约定见[仓库协作规则](AGENTS.md)。

引擎接口抽取、ReGBA 适配层与公共会话分离，以及接入 mGBA 前的回归条件见[引擎分离实施记录](docs/ReGBA引擎分离实施记录.md)。该记录分别追溯源码、RAM 验收镜像和 NOR 已安装版本，并列出每阶段实际通过与待完成的检查。

mGBA 使用单独的构建目录和存档目录。初始化 `third_party/mGBA` 后运行 `.\scripts\build-regba.ps1 -Engine mgba`；默认命令仍构建 ReGBA。此开关在编译时选择一个内核，当前不提供运行时双引擎切换。mGBA 的实际验收范围随后记录在引擎分离记录中，不能沿用 ReGBA 的板测结论。

新电脑部署时，先克隆工程并初始化两项必要的固定版本源码依赖，再准备 XBurst2 ELF R5.1.6 工具链、CMake、Ninja 和 Python：

```powershell
git clone https://github.com/PKM-Bai/x2600.git
cd x2600
git submodule update --init libbare-cpu third_party/ReGBA
python -m venv .venv
.\.venv\Scripts\python.exe -m pip install -r requirements.txt
```

工具链的安装命令及版本见本文“安装来源与重新部署”。完成依赖准备后，在仓库根目录运行 `.\scripts\build-regba.ps1`，用 `.\scripts\test-regba-host.ps1` 执行主机回归。构建和主机回归均不烧录开发板。

仅在重编原启动器或复现早期 Hello 流程时，额外运行 `git submodule update --init u-boot-ingenic`。它使用君正旧 HTTP 远程；日常 ReGBA 构建不需要初始化该子模块。`scripts/build-uboot.sh` 会自动应用仓库保存的两项 U-Boot 补丁。

源码仓库不包含商业游戏 ROM、自有 BIOS、用户存档、NOR / TF 备份、构建镜像与原始串口日志，也不打包本机工具链、Cloner 或 `Halley7/` 硬件资料。文中这类路径是开发机上的依据；可在线查阅的精选验收摘要位于 [docs/validation](docs/validation/)，JSON 中保留的本地路径用于追溯，并不表示相应原始产物已上传。ROM 和自有 BIOS 由使用者自行准备。

这套环境面向 HALLEY7-M-KIT：X2600M、64MB DDR2、SFC NOR Flash。使用君正 libbare-cpu 在 MIPS 主核上运行独立应用，启动程序负责初始化 DDR。当前应用为 [FreeRTOS + ReGBA 掌机工程](projects/halley7m-regba/README.md)，已接入中文游戏库、蓝牙手柄、原生存档、SPI 提速及屏上 CPU / FPS。

当前已安装 NOR 的 Release 为 **2306352 字节 / CRC32 `178853ea`**，所有 GUI 页面继续使用 **GBA**。新增“读取存档”和“游戏设置”：读取原生 `.sav` 须确认，复用缓存 ROM 重启后在游戏内继续游戏；设置支持音量 0～100、跳帧 0～3、速度 1× / 2× / 4×，仅会话保留，默认 100 / 0 / 1×，加速静音、回到 1× 恢复声音。

Release / Debug、完整主机回归、RAM GUI / 三类诊断存档恢复 / 32MiB 检查、NOR 全字节与启动环境核验和软件复位自动进入列表均通过。用户确认“菜单、音量、加速和声音恢复均正常”；本版未做真实断电冷启动、30 分钟长测或真实游戏具体剧情点恢复。证据见[读档与设置验证摘要](docs/validation/gui-options-20261018-validation.json)和[GUI 实施记录](docs/ReGBA掌机GUI实施记录.md)。`c77de25d` 的全 GUI 标题统一及下方中文修复验收保留各自历史版本范围。

2026-10-18 中文文件名验收基线 Release 为 **2301840 字节 / CRC32 `a85324e3`**，从旧 HUD 版 `3d721f71` 更新，完成全字节回读、启动环境完整校验、原启动代码与本次 rootfs 保留检查，并由原 U-Boot 软件复位自动启动新 GUI。用户已确认 RAM 版游戏列表中文显示正常，板端已正确读出 8 个中文 ROM 名；本版已完成中文路径游戏启动 / 保存 / 备份、32MiB 切换 / 退出和冷启动自动检查，断电后中文 `.sav` / `.bak` 完整读回一致；用户按要求断电约 5 秒再上电，并检查中文列表、龙珠加载页中文标题和游戏画面，对这四项统一回复“正常”；本版未新增物理手柄或音质专项验收。实测进展见[GUI 实施记录](docs/ReGBA掌机GUI实施记录.md)和[版本摘要](docs/validation/chinese-name-20261018-validation.json)，操作规则见[NOR 安装与更新](docs/烧录步骤.md)。原 Linux 内核起始区已替换为模拟器，原始完整 NOR 双份备份保留。

中文 ROM 文件名使用 UTF-8 应用路径，由文件任务转换为 FatFs UTF-16，并编入仓库 Unifont 的 34869 个可用 BMP 字形。代码、双构建和主机回归已通过，该中文修复基线的 NOR 更新证据为 flash-chinese-name-20261018.json（本地记录 `build/flash-chinese-name-20261018.json`）；编码长度、字形范围、当前自动检查结果及用户确认范围见 GUI 记录。此前串口消失来自用户检查 RAM 列表后断电，已重新连接，不作为固件错误。

下面的 Hello World、USB Cloner 与自编 U-Boot 内容为早期环境记录。当前 ReGBA 安装保留原 SPL / U-Boot 代码，采用串口定区写入及启动环境更新；不使用旧 Cloner 的整段覆盖配置。

本板规格、Flash 实读结果、参考底板接口、供电电平与当前时钟整理见 [HALLEY7-M-KIT 硬件规格](docs/HALLEY7-M-KIT硬件规格.md)。

Windows 原生和 WSL2 Ubuntu 22.04 都已安装官方 XBurst2 ELF R5.1.6 工具链，GCC 7.2.0。两个平台都已编译出相同的 `halley7m_hello.bin`：15,748 字节，ELF32 little endian，入口 `0x80600000`。Windows Cloner 2.5.54 已部署且能启动，其 USB 驱动已加入 Windows 驱动存储，发布名 `oem140.inf`。接板后已验证 `A108:EAEF` 的 `usb cloner device` 绑定该驱动，设备状态正常、错误码0，并成功读取USB设备描述符；实际烧录和启动输出仍需验证。

匹配 X2600M DDR2 NOR 的 `u-boot-with-spl.bin` 已成功构建，大小为198,780字节，小于 `0x3f000` 的环境区起始地址。裸机启动补丁及 GCC7 ELF 兼容补丁均保存在 `patches`，详细构建日志位于 `build\uboot-halley7m\build.log`。

## 日常使用

在 PowerShell 中编译裸机程序：

```powershell
cd E:\GitProject\X2600
.\scripts\build.ps1
```

输出：

- `build\windows-Release\halley7m_hello.bin`：原始裸机镜像。
- `build\windows-Release\halley7m_hello.elf`：调试与 ELF 检查用文件。
- `build\windows-Release\halley7m_hello.map`：链接映射。
- `build\windows-Release\compile_commands.json`：编辑器代码分析用文件。

使用 `-Configuration Debug` 生成调试版本。修改程序的入口是 [main.c](projects/halley7m-hello/main.c)。原库位于 `libbare-cpu`，示例工程独立放在 `projects\halley7m-hello`，无需修改原库。

在 WSL 中编译相同程序及本板 NOR 启动程序：

```powershell
wsl -d Ubuntu-22.04 -- bash scripts/build.sh
wsl -d Ubuntu-22.04 -- bash scripts/build-uboot.sh
```

这些命令应在 `E:\GitProject\X2600` 目录执行。裸机输出在 `build\linux-Release`，U-Boot 输出在 `build\uboot-halley7m\u-boot-with-spl.bin`。

使用 VS Code 打开本目录，按 `Ctrl+Shift+B` 执行 Windows 编译任务。在“运行任务”中可以选择 WSL 编译和 U-Boot 编译；已有 C/C++、CMake Tools、Remote WSL 插件可复用。这里的编译任务不需要 JTAG。在线单步调试还需要匹配的调试器及其连接配置。

打开烧录工具：

```powershell
.\scripts\start-cloner.ps1
```

枚举串口并监视开发板的 UART0 日志：

```powershell
.\scripts\serial-monitor.ps1
.\scripts\serial-monitor.ps1 -Port COM12
```

`COM12` 仅为命令示例，应替换为接板后出现的真实串口，默认 115200、8N1。当前 COM46 属于其他设备。串口终端使用 `Ctrl+]` 退出。

只读检查 Windows USB 驱动与 X2600 烧录协议：

```powershell
.\.venv\Scripts\python.exe scripts\usb-check.py
```

本次接板实测返回 `USB device: A108:EAEF` 和 `CPU information: X2600`，查询不下载固件或写入Flash。设备位于“libusb-win32 devices”分类。本机另外一个“未知USB设备（设备描述符请求失败）”在拔掉开发板后仍存在，已确认与本次开发板连接无关。

## NOR 镜像和启动配置

U-Boot 源码使用 `x2600m_halley7_uImage_sfc_nor` 板级目标，匹配 X2600M DDR2 64MB。它原来通过 `bootm` 启动 Linux；[启动补丁](patches/halley7m-libbare-boot.patch)改为读取裸机程序并跳转：

```text
sfcnor read 0x40000 0x600000 0x80600000;go 0x80600000
```

已单独创建 Cloner 配置 `x2600m_halley7m_libbare.cfg`，并设为当前板级配置：

| 镜像 | NOR 偏移 | 分区大小 | 文件 |
| --- | --- | --- | --- |
| U-Boot 与 SPL | `0x0` | `0x40000`，256KiB | `build\uboot-halley7m\u-boot-with-spl.bin` |
| 裸机程序 | `0x40000` | `0x600000`，6MiB | `build\windows-Release\halley7m_hello.bin` |
| rootfs | `0x640000` | 剩余空间 | 禁用 |

2026年10月14日已用 RAM 探测程序读取实板 JEDEC ID：`0x852018`。官方 Cloner 参数表识别名为普冉 `PY25Q128H`，容量 **16,777,216 字节（16MiB / 128Mbit）SPI NOR**。结果保存在 `build/flash-probe/flash-id.json`。探测只下载和运行 RAM 程序、读取 ID，没有执行 Flash 擦除、编程或状态寄存器写入。

分区擦除模式会覆盖前 `0x640000` 字节，即 **6.25MiB**，包括原有启动程序及其环境区；16MiB NOR 足够容纳这些分区。rootfs 禁用，余下 9.75MiB 不在这两项烧录范围内。配置已开启 USB 传输检查和 Flash 回读检查。

用户提供的 `Halley7` 硬件资料对应默认 NAND 参考设计：硬件手册 PDF 第17页说明默认使用 2Gbit SPI NAND，兼容 SPI NOR；核心板原理图第4页和核心板 BOM 的 `PCBA BOM` 表第45行均将 U3 标为 `GD5F2GM7UEYIG`，容量为 256MiB NAND。本次实板读取的是上述普冉 16MiB NOR，按已读取的实板类型使用 NOR 镜像和配置。

启动后的 CPU/APLL 为 1.152GHz、MPLL 为 1.8GHz、DDR 为 600MHz。Cloner 的烧录固件使用单独的 CPU 800MHz、DDR 400MHz 配置；两者对应不同阶段。

完整接线、USB Boot 模式、驱动绑定和烧录步骤见 [烧录步骤](docs/烧录步骤.md)。本次已准备镜像路径与配置，尚未写入开发板。

## 安装来源与重新部署

| 内容 | 本地目录 | 来源 |
| --- | --- | --- |
| libbare-cpu | `libbare-cpu` | [君正官方仓库](https://gitee.com/ingenic-dev/libbare-cpu)，`develop/master`，提交 `9500d2ec917a44d0d9657940e865e70f1a885849` |
| U-Boot | `u-boot-ingenic` | 官方 README 指定的 `http://git1.ingenic.com.cn:8082/gerrit/bootloader/u-boot`，`ingenic-master`，提交 `501177955aab5f844d52a0a8ea3802e704f9ceca` |
| Windows XBurst2 ELF | `toolchains\xburst2-windows` | `D:\BaiduNetdiskDownload\Xburst2\Windows\mips-ingenic-xburst2-elf-tools-x86_64-w64.r5.1.6.zip` |
| Linux XBurst2 ELF | `toolchains\xburst2-linux` | `D:\BaiduNetdiskDownload\Xburst2\Linux\mips-ingenic-xburst2-elf-tools-r5.1.6.tar.bz2` |
| Cloner | `tools\cloner-windows` | `usb_cloner_release\v2.x.x\2.5.54\cloner-2.5.54-windows_alpha.zip` |
| 串口与USB诊断工具 | `.venv` | Python 虚拟环境，`requirements.txt` 固定 PySerial 3.5 与 PyUSB 1.3.1 |

工具链包 MD5 已与君正随包校验文件比对一致：Windows `7b6038ffc0c024a5e4aca91ec037d323`，Linux `e1a6ce3b9e2f0aaac6a4e29f94ed6b6b`。两个构建脚本都会检查编译器的 XBurst2 标识。

安装脚本在已有目标目录时会停止，不覆盖当前工具链。新电脑部署时先取得同版本官方包，然后运行：

```powershell
.\scripts\install-toolchain.ps1 -ArchivePath 'D:\BaiduNetdiskDownload\Xburst2\Windows\mips-ingenic-xburst2-elf-tools-x86_64-w64.r5.1.6.zip'
wsl -d Ubuntu-22.04 -- bash scripts/install-toolchain.sh /mnt/d/BaiduNetdiskDownload/Xburst2/Linux/mips-ingenic-xburst2-elf-tools-r5.1.6.tar.bz2
.\scripts\install-cloner.ps1 -ArchivePath 'E:\GitProject\X2600\usb_cloner_release\v2.x.x\2.5.54\cloner-2.5.54-windows_alpha.zip'
.\scripts\configure-cloner.ps1
.\scripts\install-driver.ps1
python -m venv .venv
.\.venv\Scripts\python.exe -m pip install -r requirements.txt
```

`configure-cloner.ps1` 会从 [板级配置模板](configs/cloner/halley7m-libbare.cfg.in)生成本机路径，并预选 X2600M DDR2 NOR 裸机配置；重新运行会更新同名配置和当前板型。Windows 驱动安装脚本会调用系统 UAC 请求管理员权限。WSL 当前已具备 `build-essential`、`libssl-dev`、`zlib1g-dev`、`libncurses-dev` 和 Python3；新 Linux 环境需要准备这些主机依赖。

U-Boot 源码以 LF 行尾保存。构建脚本应用 [GCC7 ELF 兼容补丁](patches/uboot-gcc7-elf-compat.patch)，调整标准头文件包含顺序，并为非 SPL 汇编显式启用 PIC，保留原有 GOT 和动态重定位。旧版 SPL 构建存在并行依赖文件竞争，因此 U-Boot 脚本使用串行编译；裸机工程仍使用并行编译。

后续若要使用 X2600 的 RISC-V 小核运行现有 FreeRTOS 示例，需要另外安装君正 Victory0 RISC-V 工具链。当前两套 XBurst2 编译器面向 MIPS 主核。

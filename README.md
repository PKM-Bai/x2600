# X2600M / HALLEY7-M FreeRTOS

面向君正 X2600M / HALLEY7-M-KIT 的裸机与 FreeRTOS 开发仓库，包含基础 Hello 程序、GBA 掌机应用，以及 MIPS + RISC-V AMP 本地音视频播放器。仓库提供交叉编译、源码适配、产物核验、主机回归、MIPS 指令模拟和实板运行工具。

日常开发以 Windows + PowerShell 为入口；MIPS 应用使用官方 XBurst2 ELF 工具链，播放器小核当前通过 WSL2 Ubuntu 22.04 使用官方 Victory0 工具链。应用由 U-Boot 从 NOR 或串口加载到 DDR 后执行。

## 仓库当前状态

以下状态截至 **2026-10-20**；Flash 安装状态指本次开发使用的 HALLEY7-M-KIT，不代表克隆源码后会自动安装固件。

| 项目 | 当前能力与验证情况 | 详细说明 |
| --- | --- | --- |
| Hello 裸机示例 | Windows / WSL 交叉构建，基础串口与启动环境验证入口 | [源码](projects/halley7m-hello/main.c)、[构建脚本](scripts/build.ps1) |
| ReGBA 掌机 | MIPS FreeRTOS、ReGBA JIT、中文游戏库、蓝牙手柄、原生存档、读档与设置、CPU/FPS；已有 RAM 与历史 NOR 验收 | [掌机工程](projects/halley7m-regba/README.md)、[GUI 记录](docs/ReGBA掌机GUI实施记录.md) |
| mGBA 引擎 | 同一掌机工程的独立编译选项，固定 mGBA 0.10.5；软件、RAM 实板及独立 660 秒联合测试通过，性能限制单列 | [引擎分离记录](docs/ReGBA引擎分离实施记录.md) |
| AMP 播放器 | MIPS / RISC-V 各运行 FreeRTOS，H.264 硬解、音频主时钟、LVGL 中文三页、三键操作；支持 AVI/WAV、MP3、AAC/M4A、MP4 | [播放器工程](projects/halley7m-player/README.md)、[输入格式](projects/halley7m-player/FORMATS.md) |
| 当前实板 Flash | **播放器 R9**，NOR 包 2,326,528B / CRC32 `3d7e43a0`；完整回读、软件复位自动启动及短播放检查通过 | [当前安装与更新](projects/halley7m-player/NOR_INSTALL.md) |

播放器 R9 已修复视频空闲约 3 秒隐藏上下 UI / 进度条、任意键唤出，以及 MP4 总时长等到 EOF 才显示的问题；用户实屏确认通过，默认音量为 **7%**。暂停、菜单和播放结束时控制条常显。

播放器正在接入屏幕板载 GT911 和新版触摸界面：媒体库提供目录导航与点选，播放页使用浮动控制卡，保留实体三键。驱动从本板原始设备树核实 I2C0 / PC0、PC1、PC7、PC8；协议及真实 LVGL 主机回归通过，真机坐标与触摸操作尚待验收。当前 Flash 仍为上表的 R9，详见[触摸驱动](projects/halley7m-player/touch/README.md)及[GUI说明](projects/halley7m-player/gui/README.md)。

播放器以完整横屏1280×720 / 30fps作为目标规格。当前 MP4 支持普通横屏文件，无需电脑预旋转；采用 H.264 Baseline/Main/High、8bit YUV420 逐行视频和 AAC-LC 音频，支持真实 PTS/DTS、CABAC、B 帧、常见尺寸/SAR 及 BT.601/BT.709 有限/全范围。具体参考帧、重排、尺寸和封装边界以格式说明为准。进度跳转、字幕、HE-AAC、多声道和 1080p 尚未接入。

当前没有运行时应用启动选择器，也没有运行时 ReGBA/mGBA 引擎切换。历史 AVI 30 分钟长跑、各版模拟器验收、当前 R9 短片结果分别保留，不合并为所有版本和所有片源的性能保证。曾反馈的整屏频闪在 R8 冷暖启动复查时未复现，根因未定位；R9 本轮未做物理断电冷启。最新结果见[播放器验收记录](projects/halley7m-player/VALIDATION.md)及 [docs/validation](docs/validation/)。

## 仓库结构

```text
X2600/
├─ projects/
│  ├─ halley7m-hello/       # 基础裸机示例
│  ├─ halley7m-regba/       # 掌机应用：公共会话、GUI、平台驱动及两种引擎适配
│  │  ├─ core/             # 公共会话、游戏库、菜单与播放策略
│  │  ├─ engines/          # regba / mgba 引擎适配
│  │  └─ platform/         # 存储、显示、按键、音频、蓝牙、FatFs 等
│  └─ halley7m-player/      # 独立 AMP 播放器
│     ├─ mips/             # 主核运行时、媒体会话、子核启动与诊断
│     ├─ riscv/            # 小核 FreeRTOS 运行时与音频服务入口
│     ├─ common/           # 共享协议、媒体解析、输入及纯 C 逻辑
│     ├─ audio/            # 音频硬件服务
│     ├─ audio-decode/     # MP3 / AAC 解码、裁剪与 PCM 转换
│     ├─ codec/            # FELIX / libh264 硬解适配
│     ├─ display/          # DPU、NV12、旋转及帧所有权
│     ├─ storage/          # TF、预读与文件访问适配
│     └─ gui/              # LVGL 三页、字体与显示表面
├─ scripts/                # 环境准备、构建、核验、测试、RAM 加载与 NOR 更新
├─ tests/                  # 主机夹具、模拟器诊断与测试数据
├─ cmake/                  # XBurst2 / Victory0 交叉工具链配置
├─ configs/                # 早期 Cloner 板级配置模板
├─ patches/                # 可复现的 U-Boot 补丁
├─ docs/                   # 硬件、实施记录及精选验收摘要
├─ libbare-cpu/            # 固定版本 SDK 子模块
├─ third_party/            # ReGBA/mGBA 子模块及播放器固定音频依赖
└─ u-boot-ingenic/          # 可选启动器子模块
```

`build/`、`toolchains/`、`tools/`、`.venv/`、`Halley7/`、`usb_cloner_release/` 等为本地目录，不随源码分发。仓库没有统一的根 CMake 应用目标，请使用下面对应项目的构建脚本。

## 编译环境搭建

### 1. 克隆源码与初始化依赖

在 PowerShell 中执行：

```powershell
git clone https://github.com/PKM-Bai/x2600.git
cd x2600
git submodule update --init libbare-cpu third_party/ReGBA
python -m venv .venv
.\.venv\Scripts\python.exe -m pip install -r requirements.txt
.\.venv\Scripts\Activate.ps1
```

后续示例均在仓库根目录运行，`python` 指激活后的虚拟环境。不能激活时，可使用 `.\.venv\Scripts\python.exe` 的完整路径。Python 依赖固定为 PySerial 3.5、PyUSB 1.3.1。

只有选择 mGBA 或重编启动器时，再分别初始化对应子模块：

```powershell
git submodule update --init third_party/mGBA
git submodule update --init u-boot-ingenic
```

应用构建不需要初始化 U-Boot。其远程使用厂商旧 HTTP 地址，避免因这项可选依赖阻塞其余项目部署。

### 2. 主机工具

| 工具 | 用途与要求 |
| --- | --- |
| Git | 获取仓库与固定版本子模块 |
| Python 3 | 构建生成器、产物核验、媒体和串口工具；当前开发机为 3.14.7 |
| CMake、Ninja | ReGBA/mGBA/player 构建；工程最低 CMake 3.16，当前开发机 CMake 4.3.2 |
| MinGW Make | Windows Hello 脚本使用 `MinGW Makefiles`，需能找到 `mingw32-make` |
| Visual Studio C++ / Build Tools | **主机测试**使用 MSVC x64；安装 C++ 桌面开发工具和 Windows SDK |
| WSL2 Ubuntu 22.04 | 当前 Linux Victory0、Linux XBurst2 指令模拟器和可选 U-Boot 构建 |
| FFmpeg / FFprobe 9.0.2 essentials | 播放器转码、片源分析及完整媒体主机回归；准备脚本会核验版本 |

MSVC 编译的是主机测试程序；目标固件仍由官方交叉工具链生成。CMake、Ninja、Python 和所需 Make 应加入对应系统的 PATH。

### 3. MIPS 工具链

使用官方 **XBurst2 ELF R5.1.6 / GCC 7.2.0**，不要替换为面向 Linux 用户态的 MIPS 工具链。自行取得对应安装包后执行，示例中的 `D:\Packages` 换成本机路径：

```powershell
.\scripts\install-toolchain.ps1 -ArchivePath 'D:\Packages\mips-ingenic-xburst2-elf-tools-x86_64-w64.r5.1.6.zip'
```

默认安装到 `toolchains/xburst2-windows/`。安装脚本遇到已有目标目录会停止；已安装环境直接使用，无需重复解包。MIPS 应用采用小端、MIPS32r2、32 位 ABI；FreeRTOS 应用使用 hard-float。

如需 WSL Hello、U-Boot 或 MIPS 指令模拟，另准备 Linux R5.1.6 包。已安装 WSL2 Ubuntu 22.04 后，在 PowerShell 执行：

```powershell
wsl -d Ubuntu-22.04 -- sudo apt update
wsl -d Ubuntu-22.04 -- sudo apt install -y build-essential cmake ninja-build python3 libssl-dev zlib1g-dev libncurses-dev
wsl -d Ubuntu-22.04 -- bash scripts/install-toolchain.sh /mnt/d/Packages/mips-ingenic-xburst2-elf-tools-r5.1.6.tar.bz2
```

Linux 工具链默认在 `toolchains/xburst2-linux/`。Windows 与 Linux 的工具链目录、CMake 缓存和构建输出分开使用。

### 4. 播放器 Victory0 与媒体工具

当前验证环境使用官方 **Victory0 r1.1.1 Linux 包 / GCC 11.1.0**，由 WSL 调用；工程目标为 `rv32imc / ilp32`。归档文件名中的 `rv32emc` 不代表工程改用 E ABI，准备脚本会实际编译验证所需指令与寄存器。

```powershell
.\scripts\prepare-player-tools.ps1 `
  -RiscvArchive 'D:\Packages\riscv-ingenic-victory0-rv32emc-elf-tools-r1.1.1.tar.bz2' `
  -FfmpegArchive 'D:\Packages\ffmpeg-9.0.2-essentials_build.7z' `
  -WslDistribution Ubuntu-22.04
```

脚本处理已下载归档，不自动下载；识别 Linux / Windows Victory0，安装到 `toolchains/victory0-linux/` 或 `toolchains/victory0-windows/`，媒体工具放到 `tools/ffmpeg/`。版本、来源路径、SHA256 与最小编译结果写入 `build/player-tools/dependencies.json`。若已安装，可不传归档重新核验。

小核工程使用自有最小运行时和 `libgcc`，不依赖随包 libc 的 E ABI。只构建 MIPS 单核对照版本时可使用 `-MipsOnly`，无需 Victory0。

## 各项目编译方式

下列 PowerShell 构建均默认 `Release`，加 `-Configuration Debug` 可生成调试版本。构建只生成本地产物，不加载开发板、不写 TF 或 Flash。

### Hello 裸机示例

```powershell
.\scripts\build.ps1
.\scripts\build.ps1 -Configuration Debug
```

产物为 `build/windows-Release/halley7m_hello.{bin,elf,map}`，Debug 对应 `windows-Debug`。WSL 入口为：

```powershell
wsl -d Ubuntu-22.04 -- bash scripts/build.sh
wsl -d Ubuntu-22.04 -- bash -c 'bash scripts/build.sh "$PWD/toolchains/xburst2-linux" Debug'
```

Debug 示例通过 WSL 当前仓库的 `$PWD` 传入绝对工具链目录；手动指定目录时也应使用绝对路径。WSL 产物在 `build/linux-{Release,Debug}/`，程序入口见 [main.c](projects/halley7m-hello/main.c)。Hello 用于基础启动与串口检查，不运行 FreeRTOS。

### GBA 掌机：ReGBA / mGBA

```powershell
.\scripts\build-regba.ps1
.\scripts\build-regba.ps1 -Engine mgba
```

| 选择 | 产物目录 | 固件文件名 |
| --- | --- | --- |
| 默认 ReGBA | `build/regba-Release/` | `halley7m_regba.bin`、`.elf`、`.map` |
| mGBA | `build/mgba-Release/` | 同名，位于独立目录 |

`-Engine` 是编译时选择，共用平台与会话层；mGBA 仍使用 ReGBA 来源的公共字体和诊断资源，因此仍需初始化 `third_party/ReGBA`。构建脚本自动调用 `verify-regba.py`，并在 `build/sdcard/GBA/` 生成拷卡暂存资源。

### AMP 本地播放器

```powershell
.\scripts\build-player.ps1
.\scripts\build-player.ps1 -Configuration Debug
```

默认 `-AudioOwner riscv`，先编译、核验小核，再把小核 BIN 紧凑嵌入主核镜像。主核产物位于 `build/player-riscv-Release/mips/halley7m_player.{bin,elf,map}`；小核位于同级 `riscv-wsl/player_riscv.{bin,elf,map}`，使用 Windows Victory0 时为 `riscv/`。合包清单在 `build/player-riscv-Release/manifest.json`。

可选构建：

```powershell
.\scripts\build-player.ps1 -AudioOwner mips
.\scripts\build-player.ps1 -MipsOnly
```

前者生成 `build/player-mips-Release/`，音频服务改在 MIPS 运行；后者生成 `build/player-mips-only-Release/`，不编译或嵌入小核，RISC-V 保持复位。`-DiagnosticsOnly` 生成串口探针版本，`-ProbeMedia <本地片源>` 可嵌入受控测试媒体；普通 GUI 构建不需要嵌入片源。

`verify-player.py` 检查两核架构、入口、私有区与共享区边界、嵌入内容及 **6MiB** 合包上限。播放器的内存图和协议见[工程说明](projects/halley7m-player/README.md)。

### 可选：重编 U-Boot

```powershell
wsl -d Ubuntu-22.04 -- bash scripts/build-uboot.sh
```

需先初始化 `u-boot-ingenic` 并安装 Linux XBurst2 工具链。板级目标为 `x2600m_halley7_uImage_sfc_nor`，输出 `build/uboot-halley7m/u-boot-with-spl.bin`。脚本核对并应用仓库中两项补丁，再串行编译。日常应用更新保留当前 SPL/U-Boot，不需要重编或重新烧写它。

## 模拟运行与主机测试

### 生产逻辑主机回归

```powershell
.\scripts\test-regba-host.ps1
.\scripts\test-player-host.ps1
```

两者需要 MSVC x64，可通过 `-VisualStudioPath` 指定安装目录。ReGBA 结果主要在 `build/regba-host/`，播放器结果分布在 `build/player-host/`、GUI 和音频测试目录。播放器完整回归还使用 FFmpeg/FFprobe；先完成工具准备。

这些测试编译实际 C 逻辑，以虚拟时间、内存文件、替代 DMA/RTOS/MMIO 边界验证输入、协议、存档、媒体、会话和生命周期，不连接开发板。

### MIPS 指令模拟器：ReGBA / mGBA

先分别完成对应引擎的 Release 构建，并安装 Linux XBurst2 工具链：

```powershell
wsl -d Ubuntu-22.04 -- python3 scripts/test-regba-sim.py --timeout 45
wsl -d Ubuntu-22.04 -- python3 scripts/test-regba-sim.py --depth-regression --output build/regba-depth-sim --timeout 45
python scripts/test-mgba-sim.py --mode cpu --timeout 360
python scripts/test-mgba-sim.py --mode flash --timeout 900
```

ReGBA 推荐使用 WSL 中的官方 Linux 模拟器，Windows `run.exe` 曾发生宿主崩溃；mGBA 的 Windows 入口会转交 WSL。mGBA 还支持 `sram`、`eeprom`、`mirror`、`all` 模式。诊断 ROM 由仓库脚本生成。

这类模拟验证核心逻辑和 MIPS 指令执行，不模拟完整 X2600 板卡、FreeRTOS、缓存一致性、TF、DPU 或音频 DMA，不能用模拟耗时推算真机 FPS。

### 播放器界面与媒体模拟

```powershell
python scripts/test-player-gui.py
python projects/halley7m-player/mips/test-session.py
python scripts/test-player-source.py
```

GUI 直接运行生产 LVGL 8.3.10，以内存显示驱动和注入按键生成 PNG，默认在 `build/player-gui-host/screenshots/`，不需要 SDL。会话测试验证播放、暂停、停止、EOF、时间戳和回收；source 测试使用真实编码文件核对解析与解码。

仓库未提供完整 X2600 整机模拟器。FELIX 硬解、双核启动、邮箱中断、缓存、真实屏幕和发声仍由实板验证。

## 真机运行测试

### 硬件与 TF 准备

开发板正常供电，连接 UART0 调试口并插入 FAT32 TF 卡。当前 TF 访问使用 SPI/SSI 驱动。

| 应用 | TF 目录与资源 |
| --- | --- |
| ReGBA/mGBA | `/GBA/ROMS` 放自备 `.gba`，`/GBA/CONFIG/game_config.txt` 使用构建暂存资源；ReGBA 存档在 `/GBA/SAVES`，mGBA 在 `/GBA/SAVES/mGBA` |
| 播放器 | 当前测试资源放 `/multimedia/video` 和 `/multimedia/audio`；媒体库可以浏览目录，不限定这两个路径 |

播放器可直接播放范围内的 MP4/MP3/AAC/M4A；固定 AVI/WAV 测试媒体可用 `scripts/prepare-player-media.py` 生成，该脚本不是通用 MP4 转码入口。格式范围见 [FORMATS.md](projects/halley7m-player/FORMATS.md)。停止播放后再断电取卡；需要复制的文件由读卡器写入，构建脚本不会写真实 TF。

### 串口与 RAM 加载

```powershell
.\scripts\serial-monitor.ps1
.\scripts\serial-monitor.ps1 -Port COM52
```

第一条列出端口；当前开发机使用 **COM52、115200、8N1**，其他电脑应换成实际端口。终端用 `Ctrl+]` 退出，上传和测试前关闭占用同一串口的监视器。

先构建固件，再运行 RAM 加载脚本，监听就绪后按一次开发板复位键：

```powershell
python scripts/ramload-regba.py --port COM52 --wait-boot --execute `
  --file build/player-riscv-Release/mips/halley7m_player.bin `
  --log build/ram-player-local.log
```

该脚本名称沿用 ReGBA，但 `--file` 可选择播放器、ReGBA/mGBA 或 Hello BIN。主核装载地址固定为 `0x80600000`，上传后核对长度和 CRC，再跳转；日志应使用新文件。若当前运行播放器，先 `stop` 并确认音频关闭，也可将 `--wait-boot` 换为 `--reboot-player`，由串口命令执行软件复位。

AMP 只上传紧凑主核合包。U-Boot 仍占用高端 DDR 时，不单独向小核私有区或共享区上传；主核接管后再复制小核并释放复位。RAM 测试不改 Flash，断电后仍由已安装的 NOR 应用启动。

### 功能检查与正式板测

播放器串口可查询 `status`、`session`、`audio`、`gui`、`heap`，通过 `play <路径>`、`pause`、`resume`、`stop` 控制会话。视频播放空闲隐藏、三键、中文、目录、暂停/尾段和实际发声需同时检查；测试默认音量为7%。

| 测试入口 | 用途 |
| --- | --- |
| `scripts/test-engine-gui.py` | 掌机 GUI、设置、读档和 ROM 切换实板回归 |
| `scripts/soak-regba.py` | 掌机持续运行与存储/音频/显示统计 |
| `scripts/run-player-formats-board.py` | 播放器固定格式矩阵实板验收 |
| `scripts/run-player-codec-board.py`、`scripts/run-player-geometry-board.py` | 硬解与尺寸/颜色片源检查 |
| `scripts/run-player-board.py` | 已运行播放器的 600/1800 秒采集与 GUI 操作序列 |
| `scripts/analyze-player-board.py` | 分析板测日志、播放完成证据及单核/AMP对照 |

这些入口会实际控制当前应用，有的会写原生存档；先核对其 `--help`、对应片源和日志路径。播放器正式运行器不加载或烧写固件，要求当前音量已为7%；命令与操作顺序见[板测运行说明](scripts/RUN_PLAYER_BOARD.md)和[统计口径](scripts/PLAYER_BOARD.md)。

### NOR 安装与更新

当前实板保留原 SPL/U-Boot，自动从 NOR 读取播放器到 DDR。更新播放器使用 `scripts/flash-player-nor.py`，默认只生成和核对安装计划；显式 `--execute` 才写 Flash，需匹配实际上一版派生包和安装记录，写后完整回读并核对保护区。

当前 R9 的包、CRC、上一版参数及后续更新示例见[播放器 NOR 安装说明](projects/halley7m-player/NOR_INSTALL.md)。掌机历史安装及 USB Boot/接线说明见[烧录步骤](docs/烧录步骤.md)。早期 Cloner 整段配置属于环境实验记录，不作为当前应用更新流程；首次部署到另一块板也不能直接套用本板备份和安装身份。

## 硬件环境

| 项目 | 本次实板与软件使用情况 |
| --- | --- |
| 开发板 / SoC | HALLEY7-M-KIT / X2600M |
| 供电 | 底板5V，当前外接TF模块使用3.3V；接线按实际板版核对 |
| MIPS 主核 | XBurst2；当前启动配置 CPU/APLL 1.152GHz，MPLL 1.8GHz |
| RISC-V 小核 | Victory0；播放器已通过双 FreeRTOS 启动，计时器校验约576MHz |
| DDR | 64MiB DDR2，物理范围 `0x00000000–0x03FFFFFF`，当前 DDR CK 600MHz |
| 启动存储 | 普冉 PY25Q128H，SPI NOR 16MiB，实读 JEDEC ID `0x852018` |
| 屏幕 | 2-lane MIPI-DSI，原生720×1280、当前扫描约30Hz，横置使用1280×720；沿原Linux `zc50289hshd02` 配置，面板实物型号未据此认定 |
| 播放器显示 | FELIX H.264 → NV12 → DPU；普通横屏视频使用硬件旋转，LVGL 为独立 ARGB 图层 |
| 音频 | AIC / Codec / PDMA，播放器最终输出48kHz、S16LE、单声道；AMP默认由RISC-V独占音频硬件 |
| TF | 当前使用SPI/SSI + FatFs、FAT32，支持中文路径转换 |
| 三键 | A=PE14/SW3，B=PE15/SW4，Start=PE13/SW6；5ms扫描、20ms去抖 |
| 屏幕触摸 | GT911；I2C0 100kHz，SCL=PC0、SDA=PC1，INT=PC7、RST=PC8；当前实板触控验收待完成 |
| 调试串口 | UART0，SoC侧1.8V逻辑、115200/8N1；当前Windows枚举COM52，端口号不固定 |
| USB Boot | `A108:EAEF` Cloner设备；与日志串口是不同通道 |

主核应用入口为 `0x80600000`。播放器AMP为主核、小核和共享区分别链接及核验；MIPS负责存储、解封装、硬解、GUI与呈现，RISC-V负责PCM消费及音频硬件。更完整的参考电路、供电和Flash资料见[硬件规格](docs/HALLEY7-M-KIT硬件规格.md)，其中早期“未烧录/待验证”状态以当前项目验收更新为准。

## 依赖关系与许可证

| 依赖 | 固定版本 / 来源 | 使用关系 |
| --- | --- | --- |
| `libbare-cpu` | 子模块 `9500d2ec…`，君正SDK | Hello、掌机、播放器的平台基础；含FreeRTOS V10.5.1、HAL/LL及播放器所用LVGL 8.3.10、FELIX/libh264源 |
| `third_party/ReGBA` | 子模块 `7f379052…` | ReGBA核心/JIT、兼容配置、开放替代BIOS；mGBA复用公共字体/诊断资源，播放器复用Unifont字体 |
| `third_party/mGBA` | 子模块 `26b7884b…`，0.10.5 | 可选GBA解释器、软件渲染与音频核心 |
| `third_party/player-minimp3` | 固定 `ea99364f…`，随仓库保存源码 | 播放器MP3解码，CC0-1.0 |
| `third_party/player-faad2` | FAAD2 2.11.2，固定 `673a22a3…`，随仓库保存源码 | 播放器固定点AAC-LC解码，GPL-2.0-or-later及原Nero通知 |
| `u-boot-ingenic` | 可选子模块 `50117795…` | 重编SPL/U-Boot及早期DDR辅助流程 |
| XBurst2 / Victory0 / FFmpeg | 开发机安装，版本及校验记录在本地 | 目标编译、指令模拟、媒体准备与参考解码；不嵌入源码仓库 |

子模块完整固定提交和来源见[依赖清单](THIRD_PARTY_NOTICES.md)、[子模块配置](.gitmodules)及Git记录；音频源码清单与许可证见[解码模块说明](projects/halley7m-player/audio-decode/README.md)。`scripts/prepare-*.py` 在构建目录生成派生源码、资源和校验清单，保留上游源码及其来源，不要求手工修改固定依赖。

根 [LICENSE](LICENSE) 为GPL v2文本；各组件保留自己的许可证和后续版本授权，FreeRTOS/LVGL为MIT，mGBA为MPL-2.0，不能把整个SDK或所有第三方目录统一重新声明。商业游戏ROM、厂商BIOS转储、用户存档、NOR/TF原始备份、录像、工具链和厂商硬件资料不随仓库分发；测试使用脚本生成的诊断资源，自有ROM等由使用者准备。

## 文档与协作

- [播放器：当前功能、构建入口](projects/halley7m-player/README.md)、[格式范围](projects/halley7m-player/FORMATS.md)、[验收记录](projects/halley7m-player/VALIDATION.md)、[NOR安装](projects/halley7m-player/NOR_INSTALL.md)。
- [掌机工程](projects/halley7m-regba/README.md)、[引擎分离](docs/ReGBA引擎分离实施记录.md)、[GUI](docs/ReGBA掌机GUI实施记录.md)、[存档与音频](docs/ReGBA存档与音频实施记录.md)、[蓝牙手柄](docs/ReGBA蓝牙手柄接入.md)。
- [硬件规格](docs/HALLEY7-M-KIT硬件规格.md)、[烧录与接线](docs/烧录步骤.md)、[精选验收摘要](docs/validation/)。

阶段文档保留各自固件身份、测试范围与未完成项；当前状态优先看本页总览和最新验收条目。主机测试、指令模拟、RAM板测、NOR回读、软件复位和物理冷启动分别记证据。

每完成一个阶段或独立功能并通过相关验证后，按[仓库协作规则](AGENTS.md)创建中文标题和正文的提交，并推送至 `origin/main`。提交源码、脚本、说明及精选摘要；构建产物、原始日志、备份、媒体和工具包保留本地。

# HALLEY7-M AMP 音视频播放器

本工程面向 X2600M / 64MiB DDR2，采用 MIPS + RISC-V、双 FreeRTOS 的独立应用。目标为横置屏幕上的完整 1280×720 / 30fps 有声播放，LVGL 中文媒体库、播放、诊断三页面，以及三枚板载键操作。首版先完成 RAM 加载验收，NOR 安装另行安排。

## 当前状态

2026-10-19：两套架构 Debug/Release 已构建并通过紧凑合包核验；实板 RAM 启动、双 FreeRTOS 握手、重复启停、256KiB共享区CRC与576MHz小核计时器校准通过。FELIX 90帧跨三个GOP的可见NV12两平面CRC全部与PC一致。修正合成器四层Z顺序重复导致的黑屏后，用户确认实际画面完整、方向及颜色正常；稳态30.272fps、late=0、显示欠载与描述符mismatch=0。音频、完整会话、LVGL GUI及30分钟联合验收仍在实施。

本机使用 Windows XBurst2 R5.1.6、官方 Victory0 r1.1.1 Linux 包（Ubuntu-22.04 WSL），以及 FFmpeg/ffprobe 9.0.2 essentials。Victory0 的编译器支持 `rv32imc / ilp32`，但随包 libgcc 采用 RV32E ABI，因此本工程以自身的 ILP32 `__clzsi2` / `__udivdi3` 避免混入不兼容对象，ELF 核验器也拒绝 RVE 标记。当前板测串口为用户指定的 COM52。

音视频测试默认音量为 **10%**，见 `common/player_config.h`；自动验收不调高音量。现有诊断固件音频输出关闭，测试片 PCM 为全零。

## 工程与使用入口

- `scripts/build-player.ps1`：两个架构分别构建，小核 BIN 紧凑嵌入主核；默认音频归属为 RISC-V。
- `scripts/build-player.ps1 -MipsOnly`：只构建主核诊断程序，未嵌入小核，不启用 AMP 或音频；它不代替完整双核构建。
- `scripts/verify-player.py`：核对 ELF 架构、入口、段边界、合包内容及 6MiB 上传上限。
- `scripts/prepare-player-tools.ps1 -RiscvArchive <包路径> -FfmpegArchive <包路径>`：解包并验证已下载的官方工具，不修改系统 PATH。
- `scripts/test-player-host.ps1`：运行三键、视频布局/帧池、媒体解析、共享协议、确认超时、ILP32 除法和只读存储主机回归。
- `scripts/test-player-artifacts.py`：使用真实 ELF/BIN 及损坏副本验证架构、入口、地址边界、ABI 与字节一致性。
- `scripts/prepare-player-media.py`：转为固定 AVI/WAV，并扫描实际封装及码流；13 项测试含真实 FFmpeg 转码、SAR 与公共时间轴。
- `scripts/make-player-probe-media.py --frames 90`：生成彩条、灰阶、方向标记、帧号和静音 PCM，并导出 PC 首帧 NV12 参考 CRC。
- `scripts/check-player-video.py --board-log <逐帧日志>`：将每个硬解输出的可见 Y/UV CRC 与 PC 解码逐帧比较，拒绝缺失、重复或错误帧。

```powershell
python scripts/make-player-probe-media.py --frames 90
.\scripts\build-player.ps1 -Configuration Release -ProbeMedia build/player-fixture/probe.avi
.\scripts\build-player.ps1 -Configuration Debug
python scripts/ramload-regba.py --port COM52 --execute --wait-boot --file build/player-riscv-Release/mips/halley7m_player.bin
```

首次上传时在串口监听就绪后正常复位开发板。已运行播放器诊断程序时，可用 `--reboot-player` 替代 `--wait-boot`，先识别诊断程序再用 WDT0 复位并自动打断 U-Boot。该通路不写 NOR 或持久环境。Linux 版工具链通过 WSL 运行时，系统需要允许当前进程调用 WSL。

串口诊断命令包括 `status`、`heap`、`amp ping 123`、`amp shared`、`amp stop`、`amp start`、`ls 0:/`、`media <UTF-8路径>`、`video <帧数,0表示EOF> <AVI路径>`、`video hold <AVI路径>`、`video verify <AVI路径>`、`video stop`。`@embedded` 代表通过 `-ProbeMedia` 内嵌的受控 AVI，例如 `video 0 @embedded`；省略 `-ProbeMedia` 时不嵌入片源并清除之前的 CMake 配置。`video verify` 关闭显示调度、输出全部帧CRC供独立PC比对；`video hold` 保留末帧直到显式stop，方便人工看图。诊断播放以主核时间调度，不将它视为音频主时钟或音画同步验收。

构建目录位于仓库 `build/`，工具链、下载包和媒体产物均为本地产物。RAM 上传复用 `scripts/ramload-regba.py --file <播放器合包>` 的 YMODEM、长度及 CRC 校验。进入应用前不能单独上传高端小核/共享区，因为 U-Boot 仍占用这些地址。

## 内存与职责

| 归属 | 物理范围 | 大小 |
| --- | --- | --- |
| 原低端保留区 | `0x00000000–0x005fffff` | 6MiB |
| MIPS | `0x00600000–0x03bfffff` | 54MiB，入口 `0x80600000` |
| RISC-V | `0x03c00000–0x03dfffff` | 2MiB，入口 `0x03c00080` |
| 共享区 | `0x03e00000–0x03ffffff` | 2MiB，主核非缓存映射 `0xa3e00000` |

主核负责存储、解封装、H.264、显示、LVGL 和三键；小核负责共享 PCM 消费、AIC/Codec/主 PDMA。两核通过共享环和邮箱通信，256KiB PCM 环在共享区内，音频播放进度作为视频主时钟。

## 媒体与输入契约

视频由电脑先保持比例补黑，再顺时针预旋转为 720×1280。使用 AVI / H264 FourCC，Baseline L3.1、YUV420、30fps、单参考、无 B 帧、GOP30、Annex B、重复 SPS/PPS 和 AUD；目标/最大/VBV 为 2/3/3Mbit。音频为 48kHz / S16LE / mono PCM；独立音频使用 WAV。支持 AVI/AVIX 顺序跨段，文件小于 4GiB，首版无进度跳转。

SW3/A 与 SW4/B 在列表前后选择、播放时调节音量、诊断页滚动；按住350ms后每100ms重复。SW6/Start 短按确认或暂停/继续、诊断返回，长按800ms打开/关闭菜单，长按释放不再短按。连续短按逐次生效，无双击/三击命令。首版不使用蓝牙或触摸。

## 验收

完整验收要求 720p30 有声播放30分钟、音频欠载及DMA错误为0、无持续性视频掉帧，稳态音画偏差不超过100ms且不累积。实际输出使用同帧闪光/短音样片和手机录像核验；单核/AMP以相同10分钟窗口对照。主机测试、真实编译和实板/人工验收分别记录，不相互替代。

现有 ReGBA Release 基线重新构建为2308336字节，SHA256 `6d5a7e99a36c812776d7427c9ae3b2e8edf29666256f42ac1f3d57b8bfb378e6`；其完整主机回归通过。本次未更新 NOR。

阶段记录与证据摘要见 `VALIDATION.md`。原始串口日志、媒体、工具链和固件仅保留在 `build/` 等本地目录。

## 源码来源

FreeRTOS、X2600 HAL、LVGL 8.3.10 和 FELIX/libh264 来自 `libbare-cpu` commit `9500d2ec917a44d0d9657940e865e70f1a885849`。中文字体来源固定为 ReGBA commit `7f379052dc14366e622a96c7cf7fb6b5a7e918a3` 随附的 GNU Unifont；GUI 接入时保留对应来源声明。派生 SDK 源只生成到 `build/`，不改 SDK 原件，许可证事实见 `codec/README.md` 及原文件头。

工具归档 SHA256：Victory0 r1.1.1 为 `4c0e37045fab661438753cb2b5bacc1b80c3077f21742da5f93cc6a69cf8c694`；Gyan FFmpeg 9.0.2 essentials 7z 为 `4705843ccaaf54257c16ad90f3e952ece33c17df964ecf7bfdbb0f49c7171077`，后者已与发布方 sidecar 核对。安装脚本先成功解压并比对已安装二进制后才记录来源，拒绝 partial 下载或不匹配的归档；完整版本、配置和二进制散列见本地 `build/player-tools/dependencies.json`。

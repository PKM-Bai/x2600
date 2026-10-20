# HALLEY7-M AMP 音视频播放器

本工程面向 X2600M / 64MiB DDR2，采用 MIPS + RISC-V、双 FreeRTOS 的独立应用。目标为横置屏幕上的完整 1280×720 / 30fps 有声播放，LVGL 中文媒体库、播放、诊断三页面，以及三枚板载键操作。首版先完成 RAM 加载验收，NOR 安装另行安排。

## 当前状态

已支持 MP3、AAC-LC/ADTS、M4A 与普通 1280×720 横屏 MP4（H.264 Baseline＋AAC）。
2026-10-20真实TF的12个MP3/AAC/M4A组合各完成约30秒播放，用户确认轮播声音正常；
两份30秒横屏MP4分别覆盖44.1/48kHz双声道AAC，各900帧全呈现、drop/late0、
1440000有效音频样本准确且全部音频错误0。普通横屏MP4无需电脑预旋转。
宽幅路线切回WAV尾段、native大AVIX及停止回收已通过。编码范围及入口见
[FORMATS.md](FORMATS.md)，旋转修复与板测证据见[显示实板记录](display/BOARD_VALIDATION_20261020.md)。
音频十二例与视频两例对应的固件散列分别记录；下方30分钟记录属于原AVI/WAV固件，
不作为新格式的30分钟长跑证明。

2026-10-19：两架构独立构建、紧凑RAM合包、双FreeRTOS握手和256KiB共享校验通过，小核计时器实测校准为576MHz。FELIX跨3个GOP的90帧Y/UV CRC全部与PC一致；用户确认视频方向、颜色、中文媒体库和长按菜单正常。已接通音频主时钟的AVI/WAV会话、2MiB预读与2MiB包池、LVGL双表面及三键。同步片90帧全呈现，真实TF的96017样本WAV尾段和高运动片前缀暂停恢复已通过。

AMP已完成真实TF高运动片的30分钟板内稳定性验证：ENDED时86,400,000/86,400,000有效音频样本、54,000帧全部解码及呈现，drop/late和全部音频错误均为0，默认音量7%；固定GUI序列15/15轮、无abort，STOP后资源未锁住且音频inactive。采样在PLAY约3秒后才启动，CSV为1797条、1,797,734ms，未将其记作完整1800秒采样。同片源600秒单核/AMP对照也已完成，本轮AMP的MIPS忙率高于单核，具体窗口及结束阶段差异见下；大AVIX修复复测结果见下。精选证据见`VALIDATION.md`。

2GiB以上AVIX首轮解码90帧、呈现82帧，drop/late各8。针对FatFs大JUNK跳转增加LOADING阶段CLMT后，新包实板复测为90帧全呈现、drop/late0、144000有效音频样本完整且所有音频错误0；STOP后heap恢复，跨段播放呈现通过。上面的30分钟与单核对照属于CLMT修复前固件；原产物已保存于`build/player-evidence-before-fastseek/`，不将旧记录迁到新包散列。

本机使用 Windows XBurst2 R5.1.6、官方 Victory0 r1.1.1 Linux 包（Ubuntu-22.04 WSL），以及 FFmpeg/ffprobe 9.0.2 essentials。Victory0 的编译器支持 `rv32imc / ilp32`，但随包 libgcc 采用 RV32E ABI，因此本工程以自身的 ILP32 `__clzsi2` / `__udivdi3` 避免混入不兼容对象，ELF 核验器也拒绝 RVE 标记。当前板测串口为用户指定的 COM52。

音视频测试默认音量为 **7%**，见 `common/player_config.h`；2026-10-19用户试听后认为10%偏大、5%偏小，确认7%合适并设为默认。自动验收不调高音量，启动保持音频关闭。

用户已完成实体三键的连续短按、按住连发与释放、暂停/继续、菜单和诊断往返检查，确认全部正常。最终会话已停止、音频关闭，下一次播放音量设置恢复7%。用户现已取消手机录像及实际声光偏差数值验证，当前验收范围见下。

## 工程与使用入口

- `scripts/build-player.ps1`：两个架构分别构建，小核 BIN 紧凑嵌入主核；默认音频归属为 RISC-V。
- `scripts/build-player.ps1 -DiagnosticsOnly`：只运行串口探针，保留旧`video`逐帧CRC/显示诊断；正常构建默认启动三页GUI和异步会话。
- `scripts/build-player.ps1 -MipsOnly`：构建主核版本，音频服务由MIPS独占，小核保持停止；可用于单核对照，不代替完整双核构建。
- `scripts/verify-player.py`：核对 ELF 架构、入口、段边界、合包内容及 6MiB 上传上限。
- `scripts/prepare-player-tools.ps1 -RiscvArchive <包路径> -FfmpegArchive <包路径>`：解包并验证已下载的官方工具，不修改系统 PATH。
- `scripts/test-player-host.ps1`：运行三键、视频布局/帧池、媒体解析、共享协议、确认超时、ILP32 除法和只读存储主机回归。
- `scripts/test-player-artifacts.py`：使用真实 ELF/BIN 及损坏副本验证架构、入口、地址边界、ABI 与字节一致性。
- `scripts/prepare-player-media.py`：转为固定 AVI/WAV，并扫描实际封装及码流；13 项测试含真实 FFmpeg 转码、SAR 与公共时间轴。
- `scripts/make-player-probe-media.py --frames 90`：生成彩条、灰阶、方向标记、帧号和静音 PCM，并导出 PC 首帧 NV12 参考 CRC。
- `scripts/check-player-video.py --board-log <逐帧日志>`：将每个硬解输出的可见 Y/UV CRC 与 PC 解码逐帧比较，拒绝缺失、重复或错误帧。
- `projects/halley7m-player/mips/test-session.py`：真实FFmpeg片源、生产会话生命周期和33.033ms周期DPU模型。
- `scripts/test-player-gui.py`：实际LVGL的headless三页截图、双表面、真实显示driver边界与按键回调，无SDL。
- `scripts/test-player-measurement.py`：RAM一秒采样、CPU缓存、计数回绕、CSV和EOF/STOP范围验证。
- [run-player-board.py](../../scripts/run-player-board.py)：按固定顺序执行600/1800秒采集、GUI操作序列和STOP后CSV导出，保留原始UART行；用法见[运行说明](../../scripts/RUN_PLAYER_BOARD.md)。
- [analyze-player-board.py](../../scripts/analyze-player-board.py)：分析原始日志/CSV，按真实CPU窗口加权，对照单核与AMP的同一600秒窗口，另核对完整媒体结束证据。
- [analyze-player-sync.py](../../scripts/analyze-player-sync.py)：可选诊断工具，独立分析手机录像的闪光/1500Hz短音与拍手校准；用法见[声光分析说明](../../scripts/PLAYER_SYNC.md)，不以板内PTS代替实拍。

```powershell
python scripts/make-player-probe-media.py --frames 90
.\scripts\build-player.ps1 -Configuration Release -ProbeMedia build/player-fixture/probe.avi
.\scripts\build-player.ps1 -Configuration Debug
python scripts/ramload-regba.py --port COM52 --execute --wait-boot --file build/player-riscv-Release/mips/halley7m_player.bin
```

首次上传时在串口监听就绪后正常复位开发板。已运行播放器诊断程序时，可用 `--reboot-player` 替代 `--wait-boot`，先识别诊断程序再用 WDT0 复位并自动打断 U-Boot。该通路不写 NOR 或持久环境。Linux 版工具链通过 WSL 运行时，系统需要允许当前进程调用 WSL。

正常构建使用`play <UTF-8路径>`、`pause`、`resume`、`stop`、`session`，状态和资源由唯一media/storage worker管理，GUI只发送非阻塞命令。`@embedded`为显式RAM诊断路径，不出现在媒体库；例如`play @embedded`运行构建时嵌入片源的完整有声链路。省略`-ProbeMedia`时不嵌入片源并清除旧配置。`session trace`在STOP后导出512条固定RAM时序，`measure begin <秒数> <标签>`开始最多1800秒采样，`measure summary`查看摘要，STOP后`measure dump`导出CSV，长导出逐行保持AMP心跳。

至少1GiB的TF文件，以及需要样本表回读的MP4/M4A，在LOADING先建立16KiB簇链映射，容量不足时报告错误；其他小文件和RAM片源不分配映射表。`session storage`显示建图与实际定位统计，STOP后保留最近文件的记录。集中UART查询可能造成窗口外偶发跳帧，长摘要和CSV均安排在STOP确认后输出；播放性能以冻结的采样窗口和GUI操作记录核验。自动板测入口对长UTF-8命令分块发送，避免串口接收FIFO溢出。

`status`、`heap`、`gui`、`buttons`、`metrics`、`audio`提供显式诊断；`tone`按默认7%播放两秒短音。`ls`、`media`、`tf bench <路径>`等同步探针要求先STOP，不在播放过程中遍历或打印大目录。旧`video <帧数,0=EOF> <路径>`、`video hold/verify/stop`和DSI彩条仅在`-DiagnosticsOnly`启用；`video verify`关闭呈现调度输出全部帧CRC，该模式不能作为音画同步证据。

构建目录位于仓库 `build/`，工具链、下载包和媒体产物均为本地产物。RAM 上传复用 `scripts/ramload-regba.py --file <播放器合包>` 的 YMODEM、长度及 CRC 校验。进入应用前不能单独上传高端小核/共享区，因为 U-Boot 仍占用这些地址。

## 内存与职责

| 归属 | 物理范围 | 大小 |
| --- | --- | --- |
| 原低端保留区 | `0x00000000–0x005fffff` | 6MiB |
| MIPS | `0x00600000–0x03bfffff` | 54MiB，入口 `0x80600000` |
| RISC-V | `0x03c00000–0x03dfffff` | 2MiB，入口 `0x03c00080` |
| 共享区 | `0x03e00000–0x03ffffff` | 2MiB，主核非缓存映射 `0xa3e00000` |

主核负责存储、解封装、H.264、显示、LVGL 和三键；小核负责共享 PCM 消费、AIC/Codec/主 PDMA。两核通过共享环和邮箱通信，256KiB PCM 环在共享区内，音频播放进度作为视频主时钟。

## 媒体准备与输入契约

普通1280×720横屏MP4可直接播放，板端旋转到面板原生扫描方向；当前要求H.264
Baseline≤L3.1、固定30fps、单参考、无B帧/CABAC，配AAC-LC音轨。压缩音频支持
MP3、ADTS AAC-LC及M4A，8–48kHz、单/双声道，板端转换为48kHz单声道PCM。
完整限制见[FORMATS.md](FORMATS.md)，不将其他H.264 profile或任意MP4作为已支持范围。

原AVI通路仍由电脑先保持比例补黑，再顺时针预旋转为720×1280，使用H264 FourCC、
Baseline L3.1、YUV420、30fps、单参考、无B帧、GOP30、Annex B、重复SPS/PPS和AUD；
目标/最大/VBV为2/3/3Mbit，音频为48kHz/S16LE/mono PCM。独立PCM音频使用WAV。
支持AVI/AVIX顺序跨段、文件小于4GiB；进度跳转尚未接入。

普通片源用 [prepare-player-media.py](../../scripts/prepare-player-media.py) 转换为 `.avi` 或 `.wav`，也可用 `--validate-only` 检查已有产物。受控验收片源有三个独立入口，脚本只准备PC本地产物，不写TF卡：

| 准备脚本 | 默认产物与用途 |
| --- | --- |
| [prepare-sync-fixture.py](mips/prepare-sync-fixture.py) | `build/player-fixture/sync-flash-tone.avi`：3秒白闪/1500Hz短音，同帧事件，用于短链路与可选实拍诊断准备。 |
| [prepare-acceptance-fixture.py](mips/prepare-acceptance-fixture.py) | `build/player-fixture/acceptance/acceptance-30min.avi`：30分钟运动/每秒闪音片；同目录另有 `acceptance-tail-96017.wav` 验证音频尾段。 |
| [prepare-avix-fixture.py](mips/prepare-avix-fixture.py) | `build/player-fixture/avix/sync-flash-tone-over-2gib.avi`：3秒黑底，帧15/45/75（0.5/1.5/2.5秒）各白闪1帧，共90帧/144000样本；体积来自合法JUNK区，用于跨1GiB与2GiB的AVI/AVIX偏移测试，常见黑画面符合片源内容。 |

```powershell
python projects/halley7m-player/mips/prepare-sync-fixture.py
python projects/halley7m-player/mips/prepare-acceptance-fixture.py
python projects/halley7m-player/mips/prepare-avix-fixture.py
```

当前TF目录为 `0:/multimedia/video/` 放AVI、MP4，`0:/multimedia/audio/` 放WAV、MP3、AAC、M4A；正式旧格式长片路径为 `0:/multimedia/video/acceptance-30min.avi`。生成后需另行拷卡，再通过媒体库或 `play <路径>` 打开。大AVIX产物在PC上使用稀疏JUNK区，拷到FAT32后会实际占用2GiB以上；现已实板完成跨2GiB扫描，首轮掉帧问题经CLMT修复后复测90/90、drop/late0，原始失败及复测均保留在`VALIDATION.md`。

SW3/A 与 SW4/B 在列表前后选择、播放时调节音量、诊断页滚动；按住350ms后每100ms重复。SW6/Start 短按确认或暂停/继续、诊断返回，长按800ms打开/关闭菜单，长按释放不再短按。连续短按逐次生效，无双击/三击命令。首版不使用蓝牙或触摸。

## 验收

当前验收目标为 720p30 有声播放30分钟、保持基本音画同步和连续稳定播放、音频欠载及DMA错误为0、无持续性视频掉帧；单核/AMP以相同10分钟窗口对照。按用户2026-10-19的最新决定，取消手机录像及实际声光偏差数值验证，未做实拍不再是交付阻塞或未通过条件。主机测试、真实编译和实板/人工验收分别记录，不相互替代。

已完成同片源、7%音量和相同GUI序列的测量相对时间0..600秒对照：

| 观测 | AudioOwner=mips | AudioOwner=riscv（AMP） |
| --- | ---: | ---: |
| MIPS忙率 / 有效CPU窗口 | 37.074% / 599.283s | 39.941% / 599.235s |
| RV忙率 / 有效CPU窗口 | 保持reset，无窗口 | 1.753% / 599.472s |
| 最低观测MIPS空闲堆 | 31,665,224B | 31,661,128B |
| 冻结窗口drop / late / 音频错误 | 0 / 0 / 0 | 0 / 0 / 0 |
| 前600秒GUI序列 | 5/5轮、无abort | 5/5轮、无abort |

CPU按各核真实`window_us`加权，口径为scheduler idle，ISR归被打断任务，两核窗口边界不同步。本轮AMP的MIPS忙率增加约2.867个百分点、最低观测空闲堆少4096B；AMP约每秒10次进度事件，收发差分相等，单核无IPC。结果不支持AMP降低MIPS负载或提升稳定性的结论。MIPS测量冻结后，长CLI摘要与STOP阶段另出现drop1/late1、max_late53333us，已保留在原始日志；其冻结CSV仍为0，不将结束阶段问题隐藏或混入600秒窗口。采集器已将长摘要移到IDLE后，12项主机测试通过，尚未用修正顺序重新板测。

正式采集先在STOP状态单独完成 `tf bench`，再运行固定7%音量的采集器。单核与AMP分别加载对应固件，以相同片源、`--seconds 600`、不同label/新日志进行对照；完整长片使用1800秒。采集器每60秒仅查询短状态，结束前保留ENDED证据，停止媒体后再导出CSV，不周期打印CPU摘要。运行期间不要按实体键，实体键中止GUI序列会使该次采集无效。

```powershell
python scripts/run-player-board.py --port COM52 --media 0:/multimedia/video/acceptance-30min.avi --seconds 1800 --label amp30 --log build/player-amp30.log
python scripts/analyze-player-board.py build/player-amp30.log --window 600 --output build/player-board-analysis/amp30.json
python scripts/analyze-player-board.py --compare build/player-mips10.log build/player-amp10.log --window 600 --output build/player-board-analysis/comparison.json
```

runner的 `valid=true` 表示采集流程完整；drop/late保留实际数字，非零标记 `video_acceptance=needs-review`，由日志窗口与连续性分析判断，不自动宣称720p30通过。当前已有上述AMP 30分钟板内稳定性及600秒对照记录；MIPS窗口结束后的掉帧与修正采集顺序后的复测仍单独记录。

实际声光偏差尚未实测，不声明≤100ms已通过。原计划的1/15/29分钟手机录像及拍手校准已由用户取消必测；声光分析脚本、片源和合成回归保留为可选诊断，板内PTS和计数仍不作为实际声光偏差数值。

现有 ReGBA Release 基线重新构建为2308336字节，SHA256 `6d5a7e99a36c812776d7427c9ae3b2e8edf29666256f42ac1f3d57b8bfb378e6`；其完整主机回归通过。本次未更新 NOR。

阶段记录与证据摘要见 `VALIDATION.md`。原始串口日志、媒体、工具链和固件仅保留在 `build/` 等本地目录。

## 源码来源

FreeRTOS、X2600 HAL、LVGL 8.3.10 和 FELIX/libh264 来自 `libbare-cpu` commit `9500d2ec917a44d0d9657940e865e70f1a885849`。中文字体来源固定为 ReGBA commit `7f379052dc14366e622a96c7cf7fb6b5a7e918a3` 随附的 GNU Unifont；GUI 接入时保留对应来源声明。派生 SDK 源只生成到 `build/`，不改 SDK 原件，许可证事实见 `codec/README.md` 及原文件头。

MP3/AAC 解码依赖固定的 minimp3 与 FAAD2 2.11.2，完整来源、逐文件摘要、
CC0/GPL 许可证和 Nero 通知随 `third_party/player-minimp3`、
`third_party/player-faad2` 保留，集成与验证见 [audio-decode/README.md](audio-decode/README.md)。

Code from FAAD2 is copyright (c) Nero AG, www.nero.com

工具归档 SHA256：Victory0 r1.1.1 为 `4c0e37045fab661438753cb2b5bacc1b80c3077f21742da5f93cc6a69cf8c694`；Gyan FFmpeg 9.0.2 essentials 7z 为 `4705843ccaaf54257c16ad90f3e952ece33c17df964ecf7bfdbb0f49c7171077`，后者已与发布方 sidecar 核对。安装脚本先成功解压并比对已安装二进制后才记录来源，拒绝 partial 下载或不匹配的归档；完整版本、配置和二进制散列见本地 `build/player-tools/dependencies.json`。

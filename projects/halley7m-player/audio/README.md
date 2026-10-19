# 本地播放器音频服务

`player_audio` 是单任务拥有的 AIC / 内部 ICODEC / 主 PDMA 服务。正式双核固件仅在 RISC-V 链接并初始化；`AudioOwner=mips` 单核对照使用同一实现，并由启动层保持 RISC-V reset。父工程必须只给被选中的 owner 添加本目录，不能让两个核分别调用 `HAL_DMA_Init`。

本目录没有创建任务、访问串口、处理共享地址或修改 AMP 协议。`open/close` 会读改写 CPM 时钟寄存器，调用者必须在整个调用期间持有两核时钟握手。主 PDMA 由音频独占；SDK `HAL_DMA_Init` 会停止其全部通道。

## 固定输入与生命周期

- 输入为 48 kHz、单声道、S16LE。`write` 复制到私有存储后返回实际接收的 sample 数；返回 0 表示需服务或等待空间，不能丢弃未接收部分。
- 六个 period，每个 480 samples / 10 ms。首次播放、饥饿恢复等待五个完整 period；EOF 允许不足五个立即起播。DMA 单次描述符不循环，已 READY 的后继直接在 DMA ISR 接续，任务通知延迟不会阻塞接续。
- 默认音量来自 `common/player_config.h`，当前按用户试听确认设为 7%：10% 偏大、5% 偏小。初始化、无 PCM、PAUSE、STOP 和错误状态均保持 DAC mute、PE17 功放关闭。0% 保持 DMA 流程并静音。
- `open` 要求调度器已运行，`begin` 包括 DAC 模拟电路 300 ms 稳定等待；两者应由服务任务调用。所有后续 API 也由同一任务调用；ISR 是唯一另一访问方。
- 调用者至少每 10 ms 调用 `service`，背压等待期间也要调用。ISR 会通知 `open` 时的任务，但不要求特定通知接收方式。DMA 或 FIFO 排空超过 50 ms 进入 FAULT。
- `pause` 仅提出请求；当前 period 完成后禁止取后继，等待 FIFO 空，再等待至少一个 tick 让最后的串行 sample 离开，才发布 PAUSED。此时才能确认暂停。READY / 部分填充 PCM / EOF 均保留。`resume` 只接受 PAUSED。
- `finish` 封闭最后的部分 period。DMA 以 16 samples / 32 bytes 突发，末尾补零到突发边界，并至少保留 32 samples，供首次 FIFO 预装 16 samples 后仍有一组 DMA。最多补 31 个零，`valid_samples` 始终独立于 `dma_samples`。空音轨不启动 DMA。
- `completed_samples` 仅累计成功 DMA period 的有效 samples，表示已交给 FIFO；它不是最后一个 sample 实际播放完的精细时钟。DRAINED 则要求 EOF、所有有效 period 完成、FIFO 和最后串行 sample 排空。
- `stop` 立即静音、关闭功放和重放，先单独清 CTE，再清终止标志及 W0C descriptor IRQ。确认 CTE 清零后才丢弃队列，保留计数供 ACK；失败保持 FAULT 和私有槽，`begin` 拒绝复用。调用者也必须在成功 STOP ACK 后才能回收共享环。

对外声明见 `player_audio.h`。典型顺序为 `open → set_volume → begin → write/service → finish → service直到DRAINED → stop → close`；暂停可插在播放期间。CPM 握手只覆盖硬件 `open/close`，无需在持续播放期间持有。

## 实现来源与构建

直接编译 SDK commit `9500d2ec917a44d0d9657940e865e70f1a885849` 的 `libbare-cpu/drivers/drivers-x2600/src/` 中 `x2600_ll_cpm.c`、`x2600_ll_gpio.c`、`x2600_hal_pdma.c`、`x2600_hal_aic.c`、`x2600_hal_icodec.c`，不改 SDK 文件。这些源文件头部标明 Ingenic Copyright 与 GPL-2.0-or-later，许可证来源以 SDK 文件为准。

引脚、12.288 MHz 分频、内部 Codec master、AIC FIFO 预装、PDMA CTE 分离清零顺序参考仓库已运行的 `projects/halley7m-regba/platform/audio.c`；播放器不引入其重采样、音调或 64-bit 微秒调度逻辑。

本目录可独立配置，也可由父工程 `add_subdirectory(audio)` 后链接 `player_audio`。`PLAYER_AUDIO_CORE` 默认取 `CMAKE_SYSTEM_PROCESSOR`，支持 `mips` 和 `riscv32`。RV 所有寄存器源、队列和适配器统一为 `-march=rv32imc -mabi=ilp32 -mno-relax`。不链接供应工具链的 RV32E `libgcc.a`；主固件仍须检查最终 ELF 和所拉入的每个运行库成员。

主机测试（Windows MSVC）：

```powershell
cmake -S projects/halley7m-player/audio -B build/player-audio-host -G "Visual Studio 18 2026" -A x64 -DPLAYER_AUDIO_HOST_TEST=ON
cmake --build build/player-audio-host --config Release
ctest --test-dir build/player-audio-host -C Release --output-on-failure
```

RV 静态库探针（已安装的 WSL Ubuntu-22.04 中）：

```sh
cmake -S /mnt/e/GitProject/X2600/projects/halley7m-player/audio -B /mnt/e/GitProject/X2600/build/player-audio-rv -DCMAKE_TOOLCHAIN_FILE=/mnt/e/GitProject/X2600/cmake/victory0-elf.cmake -DCROSS_COMPILE=/mnt/e/GitProject/X2600/toolchains/victory0-linux/bin/riscv32-ingenicv0-elf- -DCMAKE_BUILD_TYPE=Release
cmake --build /mnt/e/GitProject/X2600/build/player-audio-rv -j4
```

## 已验证与硬件待验

2026-10-19 验证结果：

- MSVC 19.50 `/W4 /WX`：11,129 个检查通过，覆盖全部 1..480 samples 短尾的数据和补零、有效计数、背压、环绕、部分填充、EOF 幂等，以及由 owner 停止取后继时的队列保留和 STOP reset。
- Victory0 GCC 11.1.0：三个 RV 静态库真实编译通过，七个对象的 ELF flags 均为 `0x1`（RVC / soft-float），没有 RVE bit `0x8`。全库 undefined 符号没有 64-bit / 浮点编译器 helper。
- 使用已构建的真实 RV FreeRTOS 固件对象保留全部公开 audio API，单独链接 `build/player-audio-rv/audio-link-probe.elf`；不传 `-lgcc`，无未解析符号，最终 ELF flags `0x1`，DDR 使用 191,360 bytes。该探针仅链接验证，没有更改或下载正式固件。
- XBurst2 GCC 7.2.0：同一服务及五个 SDK 源的 MIPS32r2/O32 静态库真实编译通过，输出 `build/player-audio-mips`。

基础音频已由完整 AMP 固件实板验证：用户实际听到短音并确认默认7%合适；7%两秒500Hz短音完成96,000有效samples。暂停于24,480 samples冻结100ms，前后计数一致，恢复后完成96,000 samples。1、15、16、17、479、480、481及5,003 samples短尾均完成，补零未计入有效样本；各次STOP=0，DMA错误/超时、队列欠载、FIFO欠载均为0。证据见本地`build/player-tone-volume7-board.log`、`build/player-audio-volume7-lifecycle-board.log`及`../VALIDATION.md`。

AMP真实TF 30分钟板内长跑现已完成：ENDED时86,400,000/86,400,000有效音频样本，underrun、DMA错误/超时、队列欠载和FIFO欠载均为0，STOP后音频inactive、资源pinned0。原始证据为本地`build/player-amp30-r2.log`。测量在PLAY约3秒后才开始，CSV为1797条、1,797,734ms，不记作完整1800秒采样；完整媒体播放计数另由ENDED状态确认。

基础发声、暂停/恢复、短尾和本次AMP 30分钟板内稳定性已有实板证据。同片源单核/AMP对照、错误注入后的硬件静音/资源保留和实际声光同步仍需各自验收；手机录像按用户选择后续补测，需校准FIFO / ICODEC和显示延迟，不能由完成样本计数推定声光误差。

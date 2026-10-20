# FELIX / libh264 适配

本目录负责 MIPS FreeRTOS 上的 X2600 FELIX H.264 硬解。播放器通过独立工程适配 SDK，不修改 vendor 源码。媒体解析、音频、显示和会话控制分别由对应模块负责。

## 来源与派生

固定 SDK 为 `libbare-cpu` commit `9500d2ec917a44d0d9657940e865e70f1a885849`，来源 <https://gitee.com/ingenic-dev/libbare-cpu>，原始目录为 `drivers/drivers-x2000/src/felix/libh264`。

`scripts/prepare-player-codec.py` 在构建目录生成 `derived-libh264`，对每处替换核验原始锚点；`manifest.json` 记录原始文件与派生文件 SHA256。原始 SDK 文件保持原样。派生修复包括：

- 用标准 `size_t`，为 H264Context 增加播放器 owner，输出行跨度由实际 NV12 布局提供。
- 将完整 Annex B NAL 提取为独立 RBSP，只移除一次 emulation-prevention 字节，保留 64 字节零填充。
- Slice header 直接解析完整、有界 RBSP，取消 SDK 的 32 字节临时头和第二次去转义。
- CAVLC 保持精确 bit offset；CABAC 将 header 末端向上对齐到字节。两者都从已去转义 RBSP 解码，硬件 `bs_rbsp_en=0`。
- CABAC 初始化表明确使用 `signed char`。固定 MIPS GCC 的 plain `char` 默认为 unsigned，SDK 原声明会把负的 m/n 系数解释为正数，导致错误的 CABAC 上下文。
- 修正 newlib `memalign/realloc/free` 的分配语义、AVBuffer 失败清理和日志输出边界。
- 保留底层解码错误码；FELIX 失败时在任务上下文输出 IRQ/SDE 状态和 AU/bitstream 参数，超时结束当前会话。

许可证沿用固定 SDK 的现有声明：`include/h264.h` 为 FFmpeg LGPL-2.1-or-later，`include/list.h` 标明来源于 Linux。其他私有文件未逐一提供许可证头，不把整个私有目录重新声明为 LGPL。移植参考中的 Ingenic HAL/LL 文件未直接编入本模块。仓库级说明见 `THIRD_PARTY_NOTICES.md`。

## 当前输入边界

输入为一个完整 Annex B access unit，有效长度最多 512KiB，调用者另提供至少 64 字节可写填充。一个 AU 可包含多个 NAL；首个 IDR 必须能够取得 SPS/PPS。

当前步骤 2 接受 Baseline/Main/High（profile 66/77/100）、Level 不高于 4.0、8bit YUV420、逐行帧、最多 6 个参考帧，支持 CAVLC/CABAC。FMO、MBAFF、其他位深/色度格式明确拒绝。SPS coded 尺寸仍限定为 720×1280 或 1280×720。

播放器已使用容器真实 PTS 和 duration 调度，不再要求每帧必须精确等于 1/30 秒。步骤 2 仍拒绝 CTTS/B 帧；SDK 的 B 帧输出重排修正、呈现裁剪和尾段验收由下一步骤接入。颜色矩阵/范围和其他输入尺寸也由后续步骤扩展。完整播放器支持范围以 `../FORMATS.md` 为准。

## 输出与帧池

输出是 128 字节对齐的 NV12，布局由 `player_video_layout_nv12()` 计算：

| coded 尺寸 | stride | Y 字节 | UV 字节 | 每槽字节 |
|---|---:|---:|---:|---:|
| 720×1280 | 768 | 983040 | 491520 | 1474560 |
| 1280×720 | 1280 | 921600 | 460800 | 1382400 |

帧池有 `PLAYER_VIDEO_MAX_REFS + 3 = 9` 槽，为 6 个参考帧、当前解码和两个显示持有预留空间。九个线性 NV12 面分别占 13,271,040 或 12,441,600 字节。内部参考图使用独立 tile 平面与运动信息，不能只按线性 NV12 预算内存：720p 每个内部 DPB 约 1,872,512 字节，包括 tile 1,382,400、控制 29,056 和运动信息 461,056 字节。最终以包含 GUI/旋转器/音频的实板内存记录为准。

`AVFrame` 为 SDK 私有结构。每次获取空槽建立 CODEC 持有；发布后增加 READY，仍保留参考图所需的 CODEC。提交显示把 READY 变成 PENDING；只有实际硬件完成后才变成 SCANOUT，并退休旧扫描帧。SDK 释放 DPB 仅撤销 CODEC，所有持有均消失后才可复用该槽。

## API 与时间戳

所有 codec 和帧池调用由同一媒体 owner 串行执行，目前仅允许一个 codec 实例。

| 接口 | 行为 |
|---|---|
| `player_codec_open_geometry()` | 绑定当前 session、九个有效输出缓冲和 coded 几何；默认 `open()` 包装为 720×1280。 |
| `player_codec_submit()` | 提交 AU、解码序号、真实 PTS 和 duration；填充 64 个零字节。`-EAGAIN` 时保留相同 AU，等待槽位释放。 |
| `player_codec_get_ready()` | 返回 token、平面地址、stride、AU 序号 `frame_index`、输出序号 `output_index`、`pts_us` 和 `duration_us`。 |
| `player_codec_return_frame()` | 丢弃尚未提交显示的 READY 帧，不撤销参考图或显示持有。 |
| `player_codec_drain()` | EOF 时逐次输出延迟帧；1 表示产生帧，0 表示排空，负值为错误。每次取尽 READY，持续调用至 0。 |
| `player_codec_stop()` / `close()` | 停止并复位 FELIX、释放 SDK DPB/READY；显示持有由显示 owner 确认退休后释放。 |

AU 的 PTS 在获取 AVFrame 时保存，输出不根据提交次数重新生成时间戳。`frame_index` 用于定位输入 AU；`output_index` 用于计数输出顺序，两者在 B 帧接通后会不同。停止后再调用 `player_video_pool_stop()` 更新 session，旧 token 不能访问新会话。活动调用 session 不一致返回 `-ESTALE`。

HAL NV12 枚举为 8，私有解码器为 `VPU_FORMAT_NV12=2`，通过 `player_codec_format_from_hal()` 明确转换。

## 验证入口

主机测试在 MSVC 环境运行：

```powershell
cmake -S projects/halley7m-player/codec -B build/player-codec-host -G Ninja -DPLAYER_CODEC_HOST_TEST=ON -DCMAKE_BUILD_TYPE=Release
cmake --build build/player-codec-host
ctest --test-dir build/player-codec-host --output-on-failure
```

两个测试直接使用派生代码：RBSP 测试覆盖完整边界、一次去转义、literal `00 00 03`、64 字节填充和释放；CABAC 测试以 `/J` 模拟 MIPS unsigned-char 默认值，运行实际 VDMA 描述符构建器，核对 I/P/B、三种 PB 模型、QP 0/26/51 的 45 个固定上下文值和 21 个 termination 值。

`scripts/make-player-compat-fixtures.py` 生成七组 Baseline/Main/High、CAVLC/CABAC、ref1/3/6 短片，逐组核验真实 SPS/PPS 并完整软件解码，输出每帧 Y/UV CRC。它也生成 21 秒 suite，在封闭 IDR 处依次切换七组 SPS/PPS；软件解码必须与七个独立参考片逐帧一致。

板上 `video verify @embedded` 通过统一 source 解复用，仅解码视频并从 uncached NV12 读取每帧 Y/UV CRC。它不启用音频、不提交/停止 DPU；普通 GUI 使用自身表面继续运行。只能在会话资源已停止时调用。CRC 验证与正常有声播放分开记录：前者证明像素解码，后者证明显示、时钟、音频、GUI 与内存预算。suite 的 SPS/PPS 切换验证不等于七个独立文件均已完成打开/停止验收。

`scripts/check-player-codec-crc.py --log <串口日志> --reference <NV12 CRC JSON> --report <新报告路径>` 检查一次完整验证，要求每帧两个平面一致、PTS 单调、数量完整且没有硬件错误。

2026-10-20 步骤 2 的同一 RAM 合包（SHA256 `1b0c96198c05e179448e42cd78228e69885a3e3a0a16f6d4368c956fc2a26e9f`）已通过：

- 七段 suite 共 630 帧，板内 NV12 的 1260 个平面 CRC 全部与 PC 参考一致。
- 普通 GUI 有声播放同一 21 秒 suite，解码/呈现均为 630，丢帧/迟到为 0，完成 1,008,000 个媒体样本；音频欠载、DMA/FIFO/队列错误均为 0，音量 7%。
- ENDED 时可用堆 4,546,760 字节；STOP 后释放媒体资源，可用堆 34,535,744 字节，继续保留横屏 GUI/旋转表面，`resources_pinned=0`。
- 实际 CABAC 硬解在 signed-char 修复后与 PC 逐像素一致；错误声明的旧候选在第 90 帧进入 Main/CABAC 时失败，未写入 Flash。

有声日志为 `build/player-cabac-signed-gui-play-board-20261020.log`。这些证据覆盖短片功能与一次完整生命周期，不等于新格式已完成 30 分钟长跑或七个独立文件的打开/停止回归。阶段证据入口为 `../VALIDATION.md`，Flash 仍保留此前的可用版本。

FELIX 使用 X2600 `IRQ_INTC0_FELIX`（INTC0 bit27）、CLKGR1 bit17、CPM SRBC bit28 复位，寄存器基址为物理 `0x13300000`（MIPS uncached `0xb3300000`）。每 slice 内部复位和 EArb 等待上限 10ms，解码 IRQ 等待上限 100ms；提交前清理 RBSP、描述符和 DPB 的缓存。

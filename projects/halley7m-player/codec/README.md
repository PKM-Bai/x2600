# FELIX / libh264 适配

本目录为 MIPS FreeRTOS 播放器提供 FELIX H.264 解码适配。当前已完成真实 MIPS 编译和主机侧 Annex B / RBSP 验证，硬件解码、图像正确性和 30fps 尚待板测。

## 来源与派生

源代码来自仓库固定的 `libbare-cpu` 子模块：

- 上游：<https://gitee.com/ingenic-dev/libbare-cpu>
- 固定 commit：`9500d2ec917a44d0d9657940e865e70f1a885849`
- 源目录：`drivers/drivers-x2000/src/felix/libh264`。
- 寄存器配置描述：源目录中的 `api/jzm_h264_dec.c` 与 `api/jzm_vpu.h`。

`scripts/prepare-player-codec.py` 将源目录中的 C 文件、头文件复制到构建目录的 `derived-libh264`，随后应用带锚点校验的适配。SDK 原文件保持原样；锚点不匹配会使生成失败。派生目录的 `manifest.json` 记录每个原文件和生成文件的 SHA256。CMake 跟踪 SDK 文件和生成脚本的变化，重新配置时更新派生文件。

派生改动包括：

- AVFrame 使用标准 `size_t`，增加播放器 owner 字段；修正 NV12 linesize 为行跨度，而非整平面字节数。
- 用四槽帧池 hook 分别管理 DPB 与显示持有关系，DPB 释放不再清空仍被显示持有的外部帧。
- 按完整 Annex B 边界提取 NAL，移除 emulation-prevention 字节 `03`，保留独立的 RBSP 缓冲和 64 字节零填充。SDK 原实现会在 `00 00 03` 处截断 NAL。
- Baseline / CAVLC 硬件输入统一使用上述 RBSP，取消 slice 路径中的第二次原地去转义。
- 统一 newlib `memalign` / `realloc` / `free` 的分配语义，修正 AVBuffer 分配失败清理与日志缓冲边界。
- 将 VPU 启动、等待失败传回解码调用，避免超时后仍发布错误帧。

许可证来源保留在固定 SDK 内：`include/h264.h` 明确声明其来自 FFmpeg，使用 LGPL-2.1-or-later；`include/list.h` 标明复制自 Linux 头文件。其他私有 libh264 文件未逐一提供许可证头，不能把整个私有目录统一重新声明为 LGPL。SDK 的 `x2000_ll_felix.c`、`x2000_hal_felix.c` 含 Ingenic 的 GPL-2.0-or-later 声明，它们用于移植参考，并未直接编入本模块。仓库级入口见根目录 `LICENSE` 与 `THIRD_PARTY_NOTICES.md`；派生机制保留上游现有声明，不改变上游许可证。

## 输入、输出与 API

公开接口位于 `player_codec.h`。所有 codec 调用与 `player_video_pool_t` 操作由应用的同一 owner 串行执行。当前只允许一个 codec 实例。

输入为一个完整 Annex B access unit，最大有效长度 512KiB；调用者另外提供至少 64 字节可写空间，由提交接口清零。有效长度不包含填充。片源规格为 Baseline Level 3.1、8bit YUV420、ref1、B0、CABAC0、CFR30，首 AU 带 SPS/PPS/IDR；一个 AU 可以包含多个 NAL。

输出固定为 720×1280 NV12，stride 768。每槽 Y 平面 983040 字节、UV 平面 491520 字节，总计 1474560 字节；调用者提供四个连续平面、128 字节对齐的 cached KSEG0 缓冲。内部 DPB tile、运动信息和描述符另由解码器分配。SDK HAL 的 NV12 值是 `8`，私有 libh264 的 `VPU_FORMAT_NV12` 是 `2`，通过 `player_codec_format_from_hal()` 显式转换。

| 接口 | 用途与返回约定 |
|---|---|
| `player_codec_open()` | 绑定已初始化帧池、当前 session 和四个输出缓冲；要求槽位没有遗留持有。成功返回 0。 |
| `player_codec_submit()` | 提交一个 AU 和对应帧序号；成功返回 0。`-EAGAIN` 表示当前无空闲槽，调用者保留同一个 AU，等待释放后重试。 |
| `player_codec_get_ready()` | 取出一帧 READY 元数据；没有帧时返回 `-EAGAIN`。返回 token、平面地址、stride 与提交时的帧序号。 |
| `player_codec_return_frame()` | 丢弃尚未提交显示的 READY 帧，只撤销 READY 持有。 |
| `player_codec_drain()` | EOF 时释放解码延迟帧；返回 1 表示本次产生帧，0 表示没有更多帧，负值表示错误。每轮取尽 READY 帧。 |
| `player_codec_stop()` | 停止并复位 FELIX，再释放 DPB 与 READY 持有；保留显示持有。 |
| `player_codec_close()` | 结束并释放 codec 实例；输出缓冲由调用者管理。 |

session 不匹配的活动操作返回 `-ESTALE`。开始新 session 前，先完成旧 codec 和显示的停止流程。

## 帧池顺序

`AVFrame` 是 SDK 私有结构，不能按 FFmpeg 的引用计数语义使用。codec hook 获得空槽时建立 `CODEC` 持有；发布图像增加 `READY`，仍保留 DPB 所需的 `CODEC`。

1. 初始化帧池并分配四个输出缓冲，以 `pool.session` 调用 `player_codec_open()`。
2. 提交 AU，循环获取 READY 帧。过期帧调用 `player_codec_return_frame()`。
3. 要显示的帧交给显示驱动，由 `player_video_display_submit()` 将 READY 转为 PENDING。
4. 硬件确认新帧已经扫描后，才能调用 `player_video_display_latch()`，将新帧改为 SCANOUT 并撤销旧扫描帧的显示持有。
5. SDK 的 DPB 释放通过 hook 单独撤销 CODEC 持有。一个槽的所有持有均消失后，才允许下一次解码复用。
6. 停止时先停止提交，调用 `player_codec_stop()`，再停止 DPU 并确认其不再读取缓冲，释放显示持有，关闭 codec。最后调用 `player_video_pool_stop()` 更新 session，随后复用或释放输出缓冲。

不应在 codec 或 DPU 仍持有帧时重新初始化帧池、提前推进 session，或释放输出缓冲。

## 硬件控制与验证记录

适配使用 X2600 的 `IRQ_INTC0_FELIX`，即 INTC0 的第 27 位；门控为 CLKGR1 bit17。CPM 复位依据《X2600 Programming Manual》CPM 章节的 SRBC 新表：FELIX_SR 位于 bit28，31:29 为保留位。代码只置位、清除 bit28，保留其他位。旧 FELIX / Helix 章节中的 31/30/29 停总线写法不适用于这张新表。

FELIX 地址使用芯片总地址表中的 `0x13300000`，MIPS uncached 映射为 `0xb3300000`。每 slice 的内部复位与 EArb 状态沿用 SDK API 的相对寄存器位置，等待上限 10ms；解码 IRQ 等待上限 100ms。完成信号使用 FreeRTOS ISR 信号量；启动前同步输入、RBSP、DPB 和描述符缓存。错误与超时均返回调用者。

2026-10-19 已执行：

- 使用实际 XBurst2 GCC 7.2.0 编译 `mips32r2 / o32 / fp32 / hard-float / little-endian` 静态库成功，输出位于 `build/player-codec-probe`。新适配层使用 `-Wall -Wextra -Werror`；历史 vendor C 使用独立的告警选项。
- 主机 MSVC 构建 `PLAYER_CODEC_HOST_TEST=ON`，CTest 的 RBSP 测试 1/1 通过。测试直接使用派生 C 解析器，覆盖完整 NAL 边界、SPS 转义、连续零串、AUD/SPS/PPS/IDR 顺序、64 字节零填充、缓冲复用和资源释放。
- FFmpeg 9.0.2 的媒体工具完整 13 项测试通过。实际 90 帧 AVI 的 IDR 位于 0/30/60，每个视频 chunk 含一个 AUD，各 IDR 重复 SPS/PPS；生产 C 媒体解析器接受全部真实样片。此项验证容器和片源契约，不代表 FELIX 已完成解码。

主机测试可在已初始化的 MSVC 环境中执行：

```powershell
cmake -S projects/halley7m-player/codec -B build/player-codec-host -G Ninja -DPLAYER_CODEC_HOST_TEST=ON -DCMAKE_BUILD_TYPE=Release
cmake --build build/player-codec-host
ctest --test-dir build/player-codec-host --output-on-failure
```

仍需板端验收：

- CPM 复位、每 slice 内部复位 / EArb、IRQ 确认与输出 DMA 的实际结束时序。
- FELIX 对预旋转后的 720×1280 码流支持情况，NV12 平面、stride、色彩和完整画面正确性。
- DPB 引用与 DPU 扫描交叠时四槽生命周期，以及暂停、停止、超时、重开 session 的资源回收。
- 目标 TF 码率下的持续 30fps、有声播放、音画同步和长时间稳定性。

当前编译和主机测试结果不包含这些硬件验收。

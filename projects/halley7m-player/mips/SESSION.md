# 本地播放器会话 worker

父工程将 `mips/player_session.c` 和 `common/player_session_core.c` 编入 MIPS 固件。初始化显示后，在唯一 media/storage worker 内调用 `player_session_init()`，再以不超过 5 ms 的间隔调用 `player_session_step()`。worker 同时拥有 FatFs、codec、视频池、视频显示接口、AMP 和音频客户端；不要让另一个 console 任务先调用 storage mount。建议 worker 栈至少 4096 words，目录激活包含 UTF-8 路径、FatFs 转换及解码的嵌套调用。

GUI 只使用 `common/player_session.h` 的 `post` 和 `snapshot`。八条命令队列满时返回 `-EAGAIN`，调用者稍后重试。快照通过短临界区完整复制，不借出 worker 内部地址。`request_id` 回显命令结果，目录命令使用 generation 拒绝过期窗口。`LIBRARY_ACTIVATE.value=-1` 按 worker 已处理的 MOVE/SELECT 结果激活当前选项，避免连续按键赶在 GUI 快照前时打开旧条目。

## 缓冲与时钟

会话分别分配 2 MiB 文件预读环和 2 MiB 包字节池。前者缓存文件字节并支持 RIFF/AVIX 解析器的 seek；底层读取每次最多 1024 bytes，在读取前后服务 AMP/音频。后者按 128-byte 对齐分配连续 AU 或 PCM，视频有效长度与 64-byte decoder padding 分开。音频、视频可按各自队列顺序释放，字节池只在最早存活分配释放后回收，防止音频消费覆盖尚未解码的 P 帧。

包元数据有界：64 条视频队列、256 条音频队列、512 个分配记录。预读与队列满只产生背压，不丢失媒体包。FIFO EOF、索引、跨 AVIX 和长度检查由已有 `player_media` 解析器负责。首版受控 AVI 还要求 `audio_samples == video_frames * 1600`，对应 48 kHz / CFR30 的相同时长；没有额外增加 1/2 GiB 文件限制。

LOADING 先准备首视频并等待 IRQ 确认和背光准备，再保存至少 4800 个有效 PCM samples（100 ms；短文件可提前 EOF），随后音频 START 并批量送入已保存 PCM。音频状态或已完成有效样本确认实际启动后才发布 PLAYING，避免等待较慢状态事件期间压住视频。默认音量只读取 `PLAYER_DEFAULT_VOLUME_PERCENT`，当前按用户听感试验配置为 7%。本模块没有自动音量提升。

视频以音频 `completed_samples` 为主时钟，用 `frame_index * 48000` 与 `completed_samples * 30` 的整数有理数比较。过早的 READY 帧保留；落后至少一帧时只跳过显示，AU 仍逐帧解码以维持 P 帧参考。最后一帧必须显示。暂停等待音频 PAUSED ACK，样本时钟停止后保留 READY、队列和扫描帧；恢复不重置媒体时间线。

视频已提交而尚未收到 EOD 时，允许用另一个空槽解码下一帧；最多保留一个 READY，四槽的 DPB/PENDING/SCANOUT 持有仍分别核对。只有 PLAYING 时声明 `player_display_set_video_active(true)`，GUI overlay 排队并随下一视频提交合并，避免诊断页 10 Hz 重绘独占下一显示周期。LOADING、暂停、结束及停止时恢复 GUI 独立提交。PTS 提交条件没有提前一个刷新周期。

## EOF、STOP 与错误

EOF 先把全部 PCM 交给音频，再发送 EOF；等待有效样本总数完成、音频 DRAINED、codec drain、所有视频包处理及最后显示确认后进入 ENDED。最后 NV12 scanout 和 codec/文件资源保留，直到用户 STOP 或另选文件。

STOP 必须先获得音频客户端成功停止确认，随后才能停止 FELIX、撤除视频层、收回视频内存。撤层使用 `player_display_clear_video`，保留 GUI 层。EOD 退休 completion 消费完成、所有 PENDING/SCANOUT hold 清除后，才使视频池 session 失效并释放四个 NV12 缓冲。GUI overlay 临时占用显示提交时，视频保留 READY 并等待；GUI-only completion 从不触发视频 latch。

音频、codec 或 DPU 停止失败时，所有分配保持可达，快照标记 `resources_pinned`，拒绝新 PLAY。只有用户显式 STOP 才再次尝试清理；音频协议已经要求整板复位的错误仍遵循客户端约束。媒体读取/格式/硬件错误进入 ERROR，静音并报告阶段与原因，不自动重播或重挂载。

## 目录窗口

`player_storage_dir_open/next/close` 与媒体文件共用单 worker。GUI 不访问卡。目录只显示子目录及大小写不敏感的 AVI/WAV；非根目录包含虚拟 `..`。快照保存十条窗口、全局 selected/offset/total 和 total_known，不按固定文件数截断目录。

播放/LOADING 每轮推进一个目录条目；空闲、暂停和 ENDED 最多推进十六个，每条之间服务 AMP。切换窗口必要时重新从目录开始扫描，因此内存不随目录总数增加；超大目录的后页需要等待异步扫描。总数在扫描结束前表示已发现条目数，结束后 total_known 为真。

## 验证范围

`python projects/halley7m-player/mips/test-session.py` 用生产 session/core/parser/pool C 代码与确定性 storage/audio/codec/display peers 运行；FFmpeg 生成真实 12 帧和 90 帧 H264+PCM AVI。MSVC 19.50 `/W4 /WX` 已通过：

- WAV 19201 有效 samples 的部分尾部、暂停时钟冻结、恢复与 EOF。
- AVI 全部 12 帧继续解码、迟帧只跳显示、保留真正末帧。
- 90 帧片源在 33.033 ms 周期 EOD 模型中全部解码和显示，零丢帧；时序记录确认待显期间已预解码下一帧。这个模型不代替真实显示与音频延迟验收。
- 音频先 STOP 的释放顺序、音频/codec 停止失败保留分配并拒绝新 PLAY、显式 STOP 恢复。
- 命令队列背压、LOADING 内 STOP 取消、连续 MOVE+ACTIVATE 的命令顺序。
- 26 条目录的分页、末页六条、中文路径底层回归、父目录项。
- 预读回绕、短读/EOF、接近 4 GiB 的文件偏移、取消时停止底层 IO；压缩池按序回收与跨 32-bit 计数回绕；CFR30/48 kHz 时间边界。

`mips/session-probe` 可用 `cmake/xburst2-elf.cmake` 独立构建，XBurst2 GCC 7.2.0 的两个生产对象 `/Wall /Wextra /Werror` 编译通过。扩展的 storage 游标回归也通过。上述 peers 不验证实际 SD 带宽、音频/视频 DMA 或 IRQ；完整固件、TF 连播、音画延迟与 GUI 并发仍需后续板测。本模块开发没有访问串口。

快照另发布真实的音/视频包队列长度、未送入音频客户端的 PCM samples，以及按实际提交/丢弃决策记录的 late_frames/max_late_us。后者以音频 DMA 样本时钟比较帧 PTS，迟到至少一帧计数；它不是经过 ICODEC/FIFO 延迟校准的声光误差或精细 DPU IRQ 延迟。

`mips/player_session.h` 另提供 512 条、每条 48 bytes 的固定 RAM 时序环，不逐帧打印。每次新媒体成功进入会话时清空；STOP 和 ENDED 保留记录。`player_session_trace_read(after_sequence, records, capacity, &latest)` 在短临界区复制指定序号之后的记录；游标已被覆盖时从最旧保留项继续。导出者可每次读取一条，退出 API 后打印并服务 AMP，再用该条 `sequence` 继续，避免串口输出期间心跳中断。建议播放结束后导出。

记录包含状态、解码、提交、latch、丢帧、首次等 PTS/显示忙/解码忙事件，附帧号、最近音频完成样本、队列长度、四槽持有位和结果。`time_us_low` 是微秒低 32 位；LATCH 使用实际 IRQ 时间，其余使用 worker 时间；`duration_us` 对 DECODE 是解码耗时，对 LATCH 是提交至实际 IRQ 的耗时。`flags` 的 bit0 为待显，bit1 为 READY，bit2 为音频已开始，高位 `state << 8`；`holds` 每四位一个槽。它用于区分等 PTS、VPU 耗时、错过显示提交和独立 GUI 事务，尚未标定 ICODEC/FIFO 的声光延迟。

## 显式 RAM 同步片

已有父工程 `PLAYER_TEST_AVI` + `test_media.S` 机制保持不变。`PLAYER_HAVE_TEST_MEDIA=1` 时，session init 在自己的 worker 上注册 `player_test_media_start/end`；显式 `play @embedded` 跳过 TF mount，之后仍走相同预读、解析、包池、音频、codec 和显示链路。正常媒体库不列出此路径，未链接片源时返回不存在。主机已覆盖 TF mount 失败仍能完成这一会话；没有因此宣称 TF 可读。

`python projects/halley7m-player/mips/prepare-sync-fixture.py` 生成 `build/player-fixture/sync-flash-tone.avi` 与 JSON 校验报告。受控片源为 3 秒、90 帧、144000 samples；1280×720 源图按既定工具预旋转为 720×1280 H264 Baseline L3.1，CFR30/ref1/B0/AUD/repeated headers，BT.601 full，PCM48k mono S16LE。第 15/45/75 帧（0.5/1.5/2.5 秒）各亮一帧，同一帧开始播放 1600 samples 的 1500Hz 短音，PCM 峰值固定 8192。

PC 实测输出 338500 bytes，最大视频 AU 4083 bytes；Python 契约校验、FFprobe 和生产 C parser 均得到 90 帧/144000 samples。FFmpeg 解码后逐帧亮度确认仅指定三帧为白，解码 PCM 与源数据逐字节相同，峰值和短音起点精确。该文件与原 `probe.avi` 分开，RAM 包总体是否小于 6 MiB 仍由父工程最终链接产物检查。

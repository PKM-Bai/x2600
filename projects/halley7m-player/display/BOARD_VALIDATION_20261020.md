# 2026-10-20 宽幅显示 r1–r4 实板记录

本记录保留 CUSTOM writeback → Rotator → DSI 路线的四个候选及失败边界。**r4 已完成三秒探针、两例 TF 30 秒宽幅 MP4、暂停/恢复、一次固定 GUI 操作序列，以及 wide → WAV → native AVIX 回切；两例正式 MP4 均为 900 帧解码/呈现、drop/late 为 0，音频有效样本完整且全部错误计数为 0。** r1–r3 的失败仍按原候选记录；新格式证据覆盖本记录的短片与自动序列，不据此推广到长期运行或本轮完整实体按键人工验收。

## 候选身份

下表对应本地 `build/player-format-evidence/rotation-r1` 至 `rotation-r4`，各保存 ELF、BIN、MAP、manifest。已重算保存 BIN 的长度、CRC32 和 SHA256，与各自 manifest 一致。

| 候选 | MIPS BIN 字节数 | CRC32 | SHA256 |
| --- | ---: | --- | --- |
| r1 | 1,916,832 | `2bd32c36` | `27aca027e920a2b01763b323a0e1591b0463c50cd686bb1a0d6bdec1e5ef2334` |
| r2 | 1,917,840 | `6635efcb` | `70220c27b013bbf9d366b30c52150dc72f24d81617a295486a0672a07b9ebb55` |
| r3 | 1,918,400 | `92ccc832` | `32563fcef49ea8e90642278113ffa52e8627486058c182ee64ced4251d61bf97` |
| r4 | 1,920,032 | `1e71d376` | `2ff6503f4650481d6339c64c21ecbe39e9670f93cd1f1d0977ba9a8abe1553c6` |

四者使用同一 35,880B RV 镜像，CRC32 `003d092f`，SHA256 `3a603246c35129431d47f7205ae24ac80b2931d3383b14145d1d2c5be8af7feb`。RAM 加载日志保存主机 SHA、板端长度和 CRC 校验；未写 NOR，也未将主机 SHA 误记成板端 SHA 查询。r4 为 RISC-V audio owner，默认音量 7%，protocol 4 / memory 3，RISC-V 配置时钟 576MHz。

注意文件命名：`player-formats-20261020-r3-start-board.log` 中的 r3 是早期连接/上传尝试编号，实际运行的显示候选是 SHA `27aca...` 的 **rotation-r1**；与后面的 `player-formats-rotation-r3-*` 不是同一候选。以下按产物散列关联证据。

## r1：同表面重复 SOF 导致状态机失败

首轮完成一次宽幅写回后，硬件持续扫描同一表面，原状态机将重复 SOF 当作新命令缺失，返回 `-22`。会话记录 `decoded=2`、`displayed=0`、`pinned=1`，GUI 报 `GUI_ERROR result=-22 surfaces=pinned`；音频尚未 START。该轮没有完成一次视频呈现验收。

对应原始状态有 `writebacks=1`、`rot_sof=194`、`rot_eof=1`、`rot_errors=193`。后续修正允许仍受持有保护的同表面重复扫描，同时保留未知地址与资源锁定检查。

## r2：重复扫描恢复，后续写回/呈现仍卡住

r2 的 held-frame 扫描不再报错：首次查询 `rot_sof=193`、`rot_eof=192`、`held_sof=192`、`rot_errors=0`。但三秒 `@embedded` MP4 虽解码 90 帧，仅呈现 1 帧、drop/late 各 88，仍停在 PLAYING，未到 ENDED；一帧也尚未完成显示/丢弃核算。AAC 已完成 `144000/144000` 有效样本，全部音频错误为 0。

显示端只记录一次完成的 writeback，后续 GUI/视频事务不能正常推进。STOP 后音频关闭，但显示资源保留，session `pinned=1`、错误 `-116`，不能记为停止回收通过。源码复核发现 HAL 会原地保留/修改 crop、scale 与位置，native 720 宽配置污染后续 1280 宽写回；r3 针对完整裁剪、禁用缩放及 one-shot composer 状态作了修正。

同 r2 候选独立完成的十二例 MP3/ADTS/M4A 音频结果，见 [音频实板摘要](../audio-decode/BOARD_VALIDATION_20261020.md)。音频通过范围不延伸到本节失败的宽幅显示。

## r3：能够 ENDED 和显示完整末帧，稳态呈现约隔一个刷新周期

r3 的三秒宽幅 `@embedded` MP4 已到 `ENDED`：

- `decoded=90`、`displayed=47`、`dropped=43`；`late=44`，`max_late_us=66666`。late 与呈现/丢弃统计可以重叠，不能再加到 90 帧之上。
- 音频 `total=written=completed=144000`，音量 7%，underrun、DMA 错误/超时、queue/FIFO 欠载和音频 error 均为 0。
- 显示查询 `width=1280 height=720 rotated=1`、`error=0`、`rot_errors=0`；`writebacks=50`、`video_confirmed=47`。GUI-only 更新不计入视频呈现数。
- trace 最后确认帧为 index 89。用户确认**末帧完整，方向和颜色正常**。该反馈覆盖末帧静态图像；本轮 90 帧全呈现与 30fps 指标仍未通过。
- 宽幅会话本身 STOP 后回到 IDLE、`kind=0 pinned=0`，音频 CLOSED/inactive。后续 native 回切另有失败，记录在下节。

从 `player-formats-rotation-r3-trace-board.log` 的 232 条 session trace 中提取 47 次 LATCH：

| 观察 | 结果 |
| --- | --- |
| 相邻 LATCH 的 EOD IRQ 序号差 | 45 次为 2；另 1 次启动空窗为 14 |
| 相邻 LATCH 时间间隔中位数 | 66,067µs |
| 一次提交至 LATCH 的 trace 延迟 | 36,000–66,123µs |
| 单帧 decode 的 trace 耗时 | 5,253–9,518µs |

这支持“稳态每两个 EOD 才确认一帧”的定位，不能把约 15fps 的呈现归因为视频解码本身只有 15fps。首次 14 个 IRQ 的空窗位于起音频阶段，未混作稳态 66ms 间隔。

Rotator RAM 事件还记录到：有新表面排队并出现 FA/SOF 时，`RDMA_SITE` 仍落在旧表面范围，后一次 SOF 才观察到新表面。手册第 12 章分别定义 FA 为取走地址、SOF 为开始读帧、EOF 为读帧结束；`RDMA_SITE` 是当前 DDR 像素位置的 debug 寄存器，未承诺与 SOF 原子同步。单凭 FA 不能确认呈现；当前事件没有独立时间戳/EOD 序号，也不足以独自证明 SOF 与输出 EOD 的全部相位。

r3 后的修正方向是记录已取地址/已见 SOF 的候选，在 EOD 边界重读 site 绑定真实 CUSTOM 表面，同时保留 EOF 所有权，不用 EOW/FA 代替 LATCH。r4 保留单 pending 合同完成了下面的实测；没有为了掩盖 66ms 间隔而预先增加两级 pending 流水。

## r3：WAV 尾段通过，wide → WAV → native AVIX 回切失败

宽幅会话停止后，`0:/multimedia/audio/acceptance-tail-96017.wav` 到达 ENDED/DRAINED，`total=written=completed=96017`，含最后 17 个有效样本；音量 7%，全部音频错误计数为 0。

随后停止 WAV，再打开 `0:/multimedia/video/sync-flash-tone-over-2gib.avi`，在开始视频解码前失败：`decoded=0 displayed=0 samples=0/144000`，session ERROR、`pinned=1`、错误 `-116`；再次 STOP 仍返回 `-116`。音频保持前一 WAV 已关闭的状态，没有启动本次 AVIX 的音频。

错误发生后的显示记录为：

- 路由仍为 `1280×720 rotated=1`，`generation=2 phase=1 error=-116`，未完成 native geometry 回切。
- Rotator `STATUS=0x000c0081`，`WORKING=1`，`INT_MASK=0`，`rot_errors=0`。中断屏蔽不能作为 DMA 已停止的证明；该状态符合停止/排空或布局握手未完成的边界，资源继续保留。
- GUI 报 `GUI_ERROR result=-116 surfaces=pinned`，之后 `stack_free_words=0`。生产 GUI 任务在错误分支将 `gui_handle=NULL` 并主动 `vTaskDelete(NULL)`，查询在句柄为空时固定打印 0；这个 0 **不是栈溢出的证据**。同期媒体任务仍有 11,372 words 的 high-water 余量。

这里的 **`-116` 是真实 MIPS/newlib 的 `-ETIMEDOUT`**。本工具链 `include/sys/errno.h` 定义 `ETIMEDOUT=116`、`ESTALE=133`，不能套用主机测试兼容宏而将其误判为 stale token。本次 native AVIX 没有开始解码，不能据此认定其文件解析或跨 2GiB 读取失败，也不能记作 native 回归通过。

## r4：EOD 重读绑定与三秒探针

同一三秒 `@embedded` MP4 到达 ENDED，`decoded=displayed=90`、`dropped=late=0`、`max_late_us=6666`；音频 `total=written=completed=144000`，音量 7%，全部音频错误为 0。路由 `1280×720 rotated=1 phase=0 error=0`，`writebacks=93 rot_sof=193 rot_eof=192 held_sof=192 late_binds=92 rot_errors=0`。视频确认 90 帧；GUI 更新累计 26 次呈现，其中 22 次与视频合并、4 次 GUI-only，没有将 GUI-only 记作视频帧。

带时间戳及 EOD 序号的 Rotator 事件提供了具体相位证据：

| 事件 | 观察 |
| --- | --- |
| event 3 → 4 | `30088978µs` 的 FA/SOF 仍读到旧 slot 0；285µs 后 EOD 重读得到 slot 1 内偏移 20,000B，二者仍为 `eod_seq=869` |
| event 5 | 下一次 `eod_seq=870`，`queued=-1 reading=1 last=1`，新表面完成确认 |
| event 7 → 8 | 相同边界 295µs 后观察到新 slot 0 内偏移 32B，仍为 `eod_seq=872`；下一次 873 确认 `last=0` |

这些事件解释了早期 SOF ISR 中 site 仍旧、稍后 EOD 重读可绑定新表面的情况；结合 90/90 和后面的两例 900/900，r4 已恢复所测片源的 30fps 板内呈现。上述选定事件不表示手册对所有 SOF/site 读取都提供原子同步保证。

## r4：首轮正式 runner 未开始播放，节流后复测

首轮 `player-formats-mp4-r4-board.log/.json` 是主机命令发送失败，不能计入 MP4 解码结果。该 PLAY 命令连同 CR 共 75B，旧 `Console.command` 一次发送；板端 64B RX FIFO 配合每循环只读一字节、延迟一 tick 的控制台，丢失了路径末尾和终止符。五秒超时后的 cleanup `stop` 拼进未终止的路径，形成 `...-30sstop`，返回 `ENOENT=-2`，`decoded=displayed=0 samples=0/0`。响应 `SESSION_POST kind=1` 是 PLAY，不能当作 `kind=5` 的 STOP 确认；该轮 `idle_confirmed=false`，随后人工 STOP 恢复 IDLE。

主机工具修复提交 `271c881` 将 UTF-8 命令按最多 16B 分块、块间 24ms 节流，完整发送单个 CR 后才开始解析响应；同时检查 post 的 kind 和递增 request，STOP 清理必须匹配其请求及 IDLE/inactive 终态。有限 FIFO、长 UTF-8 路径、终止符、错误 kind/旧 request 与 timeout cleanup 的主机回归通过（原 runner 15 例、formats runner 10 例）。正式复测沿用相同 r4 固件，没有用更换固件解释工具发送失败。

## r4：两例 TF 30 秒 MP4 正式通过

`player-formats-mp4-r4-paced-board.json` 记录 `status=complete passed=true functional_pass=true video_realtime_pass=true`。两例位于 `0:/multimedia/video/`，均为 H.264 1280×720、30fps，AAC 双声道在 MIPS 解码/转换为 48kHz mono 后输出。runner 记录全程约 69.067 秒，包含逐例启停、33 秒静默收集及状态查询；这是**两例独立的 30 秒播放**，不称连续长跑。

| 查询项 | `format-landscape-1280x720-30fps-48000-2ch-30s.mp4` | `format-landscape-1280x720-30fps-44100-2ch-30s.mp4` |
| --- | ---: | ---: |
| 源有效音频帧数 | 1,440,000（48kHz） | 1,323,000（44.1kHz） |
| 输出 total / written / completed | 1,440,000 / 1,440,000 / 1,440,000 | 1,440,000 / 1,440,000 / 1,440,000 |
| decoded / displayed | 900 / 900 | 900 / 900 |
| dropped / late | 0 / 0 | 0 / 0 |
| max_late_us | 23,333 | 13,333 |
| session / audio / display / Rotator error | 全部 0 | 全部 0 |
| 音频 underruns / DMA errors / DMA timeouts / queue underruns / FIFO underruns | 全部 0 | 全部 0 |
| GUI rendered / presented（累计读数） | 138 / 138 | 191 / 191 |
| GUI error / dropped repeats / dropped actions | 全部 0 | 全部 0 |
| 媒体任务最小栈余量 | 11,322 words（45,288B） | 11,322 words（45,288B） |
| GUI 最小栈余量 | 2,934 words（11,736B） | 2,934 words（11,736B） |
| CLMT enabled / capacity / required / used | 1 / 4,096 / 4 / 4 | 1 / 4,096 / 4 / 4 |
| CLMT build_us / seeks / max_seek_us | 462 / 115 / 1,256 | 5 / 18 / 919 |
| 播放前 / ENDED / STOP 后空闲堆（B） | 45,760,344 / 20,961,344 / 34,699,584 | 34,699,584 / 20,961,336 / 34,699,584 |
| STOP 后相对播放前空闲堆差（B） | −11,060,760 | 0 |

两例 ENDED 时视频/音频包队列和待写 PCM 均为 0，audio 为 DRAINED、时钟有效；随后 STOP 均确认 session IDLE、`kind=0 pinned=0 error=0`，audio CLOSED/inactive、时钟无效，音量保持 7%。路由始终 `1280×720 rotated=1 phase=0`，显示与 Rotator 错误均为 0。两次查询的 `writebacks/late_binds` 为 `1025/1023`、`1930/1928`；这些和 GUI 数一样是累计读数，含 GUI-only 事务，不能直接当作每片视频帧数。

首例从 native GUI 布局切入 wide 后空闲堆减少 11,060,760B。其中三个 CUSTOM 表面的像素负载为 `3×1280×720×4=11,059,200B`，另含 1,560B 的分配/对齐开销；STOP 后宽幅 GUI 仍持有表面。因此首例不能写成“前后堆完全相同”，也不能将有明确输出所有权的常驻表面直接记为媒体泄漏。第二例在相同 wide 布局下启停，空闲堆差为 0；本轮最终 native 回切再次恢复到 45,760,344B，见文末回收结果。

本表核对了原始串口终态与 JSON；runner 中的固件/文件 SHA 是预期主机产物记录，`board_firmware_identity_verified=false`、`board_media_sha256_verified=false`。RAM 加载长度/CRC 提供关联证据，未冒充板端查询 SHA。`physical_audio_video=not-evaluated`；r3 用户确认的完整末帧不扩写成这两例逐帧人工目测。

## r4：wide → WAV → native AVIX 回切通过

在同一 r4 RAM 实例、上述正式两例之前，`player-formats-rotation-r4-native-board.log` 记录了恢复 IDLE 后的回归：

- WAV `0:/multimedia/audio/acceptance-tail-96017.wav` 到达 ENDED/DRAINED，`96017/96017`，全部音频错误为 0，包含短尾有效样本。
- WAV STOP 后打开 native `0:/multimedia/video/sync-flash-tone-over-2gib.avi`，到达 ENDED：`decoded=displayed=90 dropped=late=0 max_late_us=6666`；音频 `total=written=completed=144000`，全部音频错误为 0。
- native STOP 后 session IDLE、`kind=0 pinned=0 error=0`，audio CLOSED/inactive；空闲堆 45,760,344B，媒体任务栈余量 11,426 words。GUI `rendered=presented=85 error=0`，栈余量 2,934 words。

这覆盖了 r3 失败的 wide/native 布局回切，并为随后两例正式 TF MP4 提供 native → wide 的实际起点；不将 r3 的失败日志改写为成功。

## r4：暂停时钟冻结与恢复通过

在 44.1kHz 双声道 TF MP4 的独立一轮中，PLAY 后约 4 秒发送 pause；两组相隔约 3 秒的查询均为 session PAUSED（`state=3`）、audio PAUSED（`state=5`），会话有效样本和 `audio.completed` 固定为 192,960，`written=298701` 也未变化。此时 `total=0` 表示流式输入尚未到 EOF、总样本数未结算，不表示零长度播放。等待后 resume，最终 ENDED/DRAINED：900 帧解码/呈现、drop/late 为 0、`max_late_us=13333`，音频 `total=written=completed=1440000`，全部音频错误为 0。

STOP 后 session IDLE、`kind=0 pinned=0 error=0`，audio CLOSED/inactive，空闲堆 34,699,584B；GUI `rendered=presented=247 error=0`、栈余量 2,934 words，媒体任务栈余量 11,322 words。

该轮在 PAUSED 时发出的 `ui sequence start 35` 被既有运行门槛拒绝，`UI_SEQUENCE_START result=-1`，最终 `cycles_started=cycles_completed=0`。因此本轮证明暂停/恢复和普通 GUI 更新，**不计作 UI 操作序列通过**；独立序列结果记录在下一节。

## r4：播放中固定 GUI 操作序列完成一次

独立 `player-formats-rotation-r4-ui-cycle-board.log` 先在 PLAYING 时启动 35 秒序列（`UI_SEQUENCE_START result=0`），再 pause 约 20 秒后 resume，使菜单/诊断操作周期落在恢复后的播放阶段。最终序列 `active=0 reason=2 cycles_started=1 cycles_completed=1 elapsed_ms=35003 duration_ms=35000 keys=000 aborted=0 cancel_may_emit_short=0`，完成一次并释放注入键。

同轮 MP4 到达 ENDED，900 帧解码/呈现、drop/late 为 0、`max_late_us=13333`，音频 `total=written=completed=1440000`、全部音频错误为 0，音量保持 7%。GUI 回到 `page=1 menu=0 generation=14`，`rendered=presented=320 error=0`、重复/动作丢弃为 0，栈余量 2,920 words（11,680B）。STOP 后 IDLE、`kind=0 pinned=0`，audio CLOSED/inactive，空闲堆 34,699,584B，媒体任务栈余量 11,322 words。

这是固件固定按键注入序列的板内功能回归，不是实体按键逐项人工覆盖，也不将一次 35 秒序列记为新格式长跑。

## r4：正式 MP4 与 GUI 回归后的最终 native 回切

两例正式 MP4、暂停和独立 GUI 序列结束后，再打开同一大 AVIX。`player-formats-rotation-r4-final-native-board.log` 的 session 9 到达 ENDED，`decoded=displayed=90 dropped=late=0 max_late_us=6666`，音频 `total=written=completed=144000`、全部音频错误为 0，音量 7%。

STOP 请求 `2147483675` 被实际应用，终态 request 匹配，session IDLE、`kind=0 pinned=0 error=0`；audio CLOSED（`state=0`）、inactive、`clock_valid=0`。空闲堆恢复到 **45,760,344B**，与正式宽幅 MP4 之前的 native 空闲值完全相同，证明本轮布局回切释放了常驻 CUSTOM 池。媒体任务栈余量 11,322 words；GUI `generation=15 rendered=presented=354 error=0`、栈余量 2,920 words，最终 `page=1 menu=0`。

## 原始日志索引与 SHA256

以下摘要均按当前保存的原始文件字节重算；路径相对仓库根目录。

| 路径 | SHA256 |
| --- | --- |
| `build/player-formats-20261020-ramload-r3.log`（rotation-r1 加载） | `77b40e254f5e1d65df7ef96c3a538667f77567a8440346082e6899fc3ba0f742` |
| `build/player-formats-20261020-r3-start-board.log`（rotation-r1 失败） | `9d747b5cffa1c1c5e39da77b8e55c725bdadaf3d3b3beb12bd6e49b718dc8e28` |
| `build/player-formats-20261020-rotation-r2-ramload.log` | `cd1c594a9d214d2d89cd2aa3d1e0442912a4e081e2312117b51c621cede55d7b` |
| `build/player-formats-rotation-r2-probe-board.log` | `2c7ac4d5911bc27f36afaa61938048799c3f3bd341b09c962882bb022a09bd24` |
| `build/player-formats-rotation-r2-display-board.log` | `50b3b5ed3d85211e98111fa90225a43ac5d83c2001c5a30e269c662a3aa7d138` |
| `build/player-formats-rotation-r3-ramload.log` | `471e69458f0b62a6e60228da3521b856270b8476dab1707d17cb5f4555844087` |
| `build/player-formats-rotation-r3-probe-board.log` | `62e02a1070cc61301717bba5631a02daef581dd2c536ca12aef56ced7bb24695` |
| `build/player-formats-rotation-r3-trace-board.log` | `6bac824e49249610bbde062018fec897b017208b05a9efbd744fb9c626adf019` |
| `build/player-formats-rotation-r3-legacy-board.log` | `373dcd0e842242bf3e4ed77dc3f646ec68b157a5f0f32a76d87d7d9989d12978` |
| `build/player-formats-rotation-r3-native-error-board.log` | `1e82617383afb5bc74ed4363c5de63e404f83cd151fc2f100fe02e858a3bf09e` |
| `build/player-formats-rotation-r4-ramload.log` | `e7923a201554cbc7f0e1bfea66bda628f339e09a97ea1902f711f523e9e36e9c` |
| `build/player-formats-rotation-r4-probe-board.log` | `35c6ce35623e3e5b8a3fe204b3e14293bb2e89b8b8a966867f4f6a5af7ec2f04` |
| `build/player-formats-mp4-r4-board.log`（未节流，未开始播放） | `2c4d43c5759cfc67d1272f9872932e9fd152b33caf04a9544e03bae1a2ecd454` |
| `build/player-formats-mp4-r4-board.json`（未节流失败报告） | `48a327ec6c63e3b9131b9fd2560064c532df7d2b36ba5867589f8a5d00c68e01` |
| `build/player-formats-rotation-r4-native-board.log` | `b44b07689cc8c1069d06e55d20b6a4fd24b943314e4b494927f9b2e874e42e73` |
| `build/player-formats-mp4-r4-paced-board.log`（两例正式通过） | `d4fbb43c66cf970d359cb55241241dc37331c0c2d672458979ee1f0629c32e73` |
| `build/player-formats-mp4-r4-paced-board.json`（两例正式报告） | `4582cdfcce50f25a780b13f5dc30f8b52d80ff31a9efc469ecf7c07bd061035b` |
| `build/player-formats-rotation-r4-pause-ui-board.log`（暂停/恢复通过，UI 序列未启动） | `6a1f204f4748818e0ff8f8adf999b48f6b1bdb0a3b918044d5ee8f6364060148` |
| `build/player-formats-rotation-r4-ui-cycle-board.log`（独立 GUI 序列一次通过） | `1c878de523b6375f02dc7ab1e28734d930f88c8017bfddb576e1d08e522d3bff` |
| `build/player-formats-rotation-r4-ui-cycle-console.log`（该轮主机输出） | `12ce5ad9a1381a418d69dfbf67c960cc6bdcdc241f0ef62749ed737ccd035464` |
| `build/player-formats-rotation-r4-final-native-board.log`（最终回切与 CUSTOM 池释放） | `7f86a5a4945a8c6cfe990156dfa4b219ca50704922276ae35f6b40c648fdac2f` |

本轮新格式固件未重复完整实体按键人工验收；旧 AVI 版本已有用户“全部正常”反馈，当前新增 GUI 按键序列属自动注入。新格式本轮未做 30 分钟长跑，本文仅记录实际覆盖范围，不将旧版本音频十二例或此前 AVI 长跑移记到 r4。实际声光偏差的数值测量已由用户取消，不列为本轮待补的阻塞项。

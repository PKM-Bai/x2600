# 2026-10-20 压缩音频十二例实板证据

本轮在 TF 上逐例播放 MP3、ADTS AAC-LC、M4A，各覆盖 44.1/48kHz、mono/stereo，共 **12 个独立的约 30 秒会话**。十二例均到达 `ENDED`，最终有效样本数符合生产解码与裁剪规则，音频错误计数均为零，STOP 后恢复 IDLE 并释放本次资源。自动报告为 `status=complete`、`passed=true`。

每例之间都有 STOP、确认音频关闭和重新 PLAY；这不是连续六分钟播放测试，也不是新格式的三十分钟长跑。

## 本轮证据与候选产物

- 时间：2026-10-20 13:34:37–13:41:21（Asia/Shanghai）。约 404 秒的主机记录还包括每例等待、状态查询与启停。
- 报告：`build/player-formats-audio12-r1-board.json`，SHA256 `6d542b615d2d19ef81e274e0e9c193873026a906c4ec77cad120eb1af3c408ae`。
- 原始 UART：`build/player-formats-audio12-r1-board.log`，SHA256 `507fd8da058dba395b30c9127432869d4f7130afe0090eb8b5bea4525a6ed363`；已重算并与报告中的 raw-log 摘要一致。
- 片源清单：`build/player-fixture/formats/manifest.json`，本轮报告记录 SHA256 `f3d7c17417ce8cf03de35b5c15db153a5bfc42fa640b6decdab2578ce2110e43`。
- runner 记录的预期 MIPS 主机产物：1,917,840B，CRC32 `6635efcb`，SHA256 `70220c27b013bbf9d366b30c52150dc72f24d81617a295486a0672a07b9ebb55`；预期 RV 镜像 35,880B，SHA256 `3a603246c35129431d47f7205ae24ac80b2931d3383b14145d1d2c5be8af7feb`。

固件信息来自本轮保存的主机 manifest，runner 未从板端查询固件 SHA，也未查询 TF 文件 SHA。该候选构建包含测试时尚未提交的旋转修正；本文的通过范围仅限十二例音频，不以这些结果验收旋转或 MP4 视频。

独立上传日志`build/player-formats-20261020-rotation-r2-ramload.log`记录本次RAM上传长度1917840B及板端CRC32 `6635efcb`核验成功，关联同一主机SHA产物。候选ELF/BIN/MAP和manifest保留在本地`build/player-format-evidence/rotation-r2`，不把后续显示修正版的散列替换到本轮音频记录。

## 逐例有效样本

所有输出均为 **48kHz、S16LE、单声道**，音量保持 **7%**。下表计数同时符合 session 的 `samples=N/N` 和 audio 的 `total=written=completed=N`；已对照原始 UART 终态记录，不只引用 runner 的布尔结果。

| TF 文件（目录 `0:/multimedia/audio/`） | 源格式 | 最终有效输出样本 | 结果 |
| --- | --- | ---: | --- |
| `format-44100-1ch-30s.aac` | ADTS AAC-LC，44.1kHz mono | 1,440,008 | 通过 |
| `format-44100-1ch-30s.m4a` | M4A AAC-LC，44.1kHz mono | 1,440,000 | 通过 |
| `format-44100-1ch-30s.mp3` | MP3，44.1kHz mono | 1,440,000 | 通过 |
| `format-44100-2ch-30s.aac` | ADTS AAC-LC，44.1kHz stereo | 1,440,008 | 通过 |
| `format-44100-2ch-30s.m4a` | M4A AAC-LC，44.1kHz stereo | 1,440,000 | 通过 |
| `format-44100-2ch-30s.mp3` | MP3，44.1kHz stereo | 1,440,000 | 通过 |
| `format-48000-1ch-30s.aac` | ADTS AAC-LC，48kHz mono | 1,440,768 | 通过 |
| `format-48000-1ch-30s.m4a` | M4A AAC-LC，48kHz mono | 1,440,000 | 通过 |
| `format-48000-1ch-30s.mp3` | MP3，48kHz mono | 1,440,000 | 通过 |
| `format-48000-2ch-30s.aac` | ADTS AAC-LC，48kHz stereo | 1,440,768 | 通过 |
| `format-48000-2ch-30s.m4a` | M4A AAC-LC，48kHz stereo | 1,440,000 | 通过 |
| `format-48000-2ch-30s.mp3` | MP3，48kHz stereo | 1,440,000 | 通过 |

十二例累计 **17,281,552 个有效输出样本**，等价于 360.032333 秒的样本总量。它是不同会话的累计，不能作为同一会话连续运行的时长。

ADTS 没有 M4A edit list 的精确尾裁：FAAD2 已隐藏首个 1024 源 PCM 帧后，44.1kHz 文件剩余 `(1293−1)×1024=1,323,008` 源帧，整体有理转换得到 `floor(1,323,008×48000/44100)=1,440,008`；48kHz 文件为 `(1408−1)×1024=1,440,768`。这些尾差符合规则。M4A 抵扣已隐藏的首帧后按容器区间裁剪，MP3 按 Xing/LAME delay/padding 裁剪，均得到准确 30 秒输出。

## 错误、回收与资源观察

- 十二例的 `ENDED` 均为 session `state=4`、audio `state=7`（DRAINED），`clock_valid=1`；待处理音频包、PCM 队列均已清空。
- 每例结束及 STOP 后，`error`、`underruns`、`dma_errors`、`dma_timeouts`、`queue_underruns`、`fifo_underruns` 均为 0；GUI `error=0`。原始日志没有 `AMP_FAIL`、`AMP_FORCE_SILENCE`、`AMP_RECOVERY_REQUIRED`、`GUI_ERROR`、panic 或 assertion-failed 标记。
- 每例 STOP 后 session `state=0`、`kind=0`、`pinned=0`，audio `state=0`、`active=0`。十二例逐例启停前后空闲堆均为 **45,764,440B**，各例 `heap_free_delta=0`。
- `ENDED` 查询时记录到的最小空闲堆为 **41,491,792B**，出现在 stereo M4A。此值是查询时的观察值，不是整个解码过程的连续最小堆采样。
- 媒体任务配置 96KiB 栈；本轮最低 stack high-water 为 **11,322 words，即 45,288B 剩余**。该计数是任务自创建以来累计的最低余量，不能据后续每例相同读数反推各解码器独立栈峰值。

四例 M4A 均实际启用 CLMT：`enabled=1`、容量 4096 DWORD，`required=used=4`，构建耗时 460–463µs，各记录 29 次 seek，单例最大 seek 1231–1236µs；结果均为 0，关闭后的堆恢复。MP3/ADTS 的 CLMT 为关闭状态。这里只说明本轮四个短 M4A 文件的索引路径与回收已运行，不推广为全卡或大文件的 seek 性能。

## 听感反馈与验收范围

用户在本轮轮播期间反馈：“能听到，声音正常”。这确认了本轮轮播存在正常实际发声的整体听感反馈；没有逐一标注十二个文件的独立听音结论，也没有声学波形或输出保真度测量。runner 自身仍将物理音视频观察记为 `not-evaluated`。

本文确认的是十二种音频组合的短片实板播放、准确 EOF 计数、零错误和逐例资源回收。新格式尚未做三十分钟长跑；本记录不包含 MP4 视频/旋转验收。实际声光偏差的数值测量已由用户取消，不将其写为通过，也不再列作本轮必须补做的验收项。

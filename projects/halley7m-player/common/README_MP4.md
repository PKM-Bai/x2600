# 有界 MP4/M4A 顺序解封装

`player_mp4.c/h` 是与 FatFs、音频解码和硬件解码分开的纯 C 模块。它复用
`player_media_io` 的读取、64 位绝对定位、文件长度回调；文件仍由会话的唯一
worker 持有。模块没有任务、串口或寄存器操作，不接管或关闭底层文件。

## 实现范围

- 非 fragmented MP4/M4A；`moov` 可以位于媒体数据之前或之后。
- 一个 `avc1` H264 视频轨、一个 `mp4a` AAC-LC 音轨，或者其中任一轨。
  音频支持 ASC 声道配置 1/2；采样率、1024/960 帧长作为元数据报告。
- `stco`/`co64`、固定/逐样本 `stsz`、`stsc`、`stts`、`stss`、版本 0/1
  `ctts`，以及版本 0/1 的 movie/track duration 和编辑表。
- 多个 `mdat`、不同 track 编号、音视频完全不交织的文件布局。
- 简单 rate=1 的单个媒体 edit，或者一个前导 empty edit 加一个媒体 edit。

`moof`/`mvex`、加密或外部数据引用、`avc3`/HEVC、HE-AAC/SBR/PS、PCE 声道
配置、多个同类型媒体轨、多个 sample description、`stz2`、复杂 edit montage
和非 1 倍编辑速率会明确失败。未知非媒体 track 可以跳过。

这些是解封装范围。它们不代表当前 session 或 FELIX 硬件已经支持所有上述
时序、H264 profile 或画面尺寸。例如测试中的 Main profile/B 帧/可变帧率用于
核对容器时间戳和 PC 解码；首批板端能力仍由 session/codec 的独立 gate 决定。
普通 1280×720 的宽高会原样报告，不在 demux 内强制预旋转为 720×1280。

## 内存与读取

固定两个 track，每个 track 的六张样本表各使用一个 512 字节页缓存。
只保存表的位置、数量和游标，不为每个 sample 分配常驻索引；内存不会随影片
长度增长。配置数据和 Annex B SPS/PPS 各有 4096 字节的固定上限。`open` 会
逐条验证时长、映射和同步样本表，不加载媒体 payload。

`next` 按 edit 映射后的 DTS 在两轨间选择下一包，文件中 video/audio 的物理
先后顺序不会决定队列填充顺序。它可能定位样本表或预扫视频 NAL，因此底层
存储必须能有效执行这些只读定位；FatFs CLMT/read-ahead 策略属于 storage 和
session，不在本模块里静默切换。

单个视频包的规范化输出上限是 512 KiB，AAC AU 上限是 64 KiB。调用
`read_packet` 时，两种包的缓冲容量都须额外包含 64 字节零 padding。
`source_bytes` 是文件里的样本长度，`normalized_bytes` 和 `written` 是实际
交给解码器的长度；padding 不计入其中。

## 时间戳、编辑表和 AAC priming

`track.duration` 是 `stts` 的时长总和；`header_duration` 保留原始 `mdhd`
声明。包里的 `dts`/`pts`/`duration` 保留 track ticks 和 `timescale`。
`dts_us`/`pts_us` 将媒体 edit 起点减去，再加 empty edit 的前导时间。每个
绝对时间单独做整数换算，不通过累加四舍五入后的单帧时长推进。

编辑表的 `edit_media_time` 使用 track ticks，`empty_edit_duration` 和
`edit_duration` 使用 movie ticks，movie timebase 在 info 中另有报告。
所有源 AAC AU 都会返回，包括负时间 preroll 和结尾 padding；demux 不会
丢掉首包，也不添加假静音。调用者在解码后的源采样轴应用 edit/encoder trim，
并单独抵扣解码器已经隐藏的样本。当前 FAAD2 的首帧内建延迟由 audio-decode
接口的 `decoder_skipped_frames` 报告，不能再无条件剪一次 elst 的 1024。

带符号 CTTS 时，FFmpeg 可能为解码调度把 DTS 整体移到负值，并保持 PTS 不变。
本模块的原 DTS 忠实保留从 0 开始的 `stts` 累计轴，PTS 则应用原 composition
offset 和 edit。这一差别在测试中按固定 DTS bias 核对，逐包 PTS 仍单独比较。

## AVCC 转 Annex B

支持 1/2/4 字节 NAL 长度前缀。`next` 先读取前缀和 NAL 头，检查一个 MP4
sample 内所有 NAL 的边界，精确计算 `normalized_bytes`，调用者据此从压缩
包池分配容量。`read_packet` 输出四字节 Annex B start code，最前面统一放一个
AUD，并在 IDR 前补 avcC 中的 SPS/PPS；源 AUD 被替换，避免出现两个 AUD。
不需要另一个 512 KiB 的临时拷贝区。

MP4 中的一个视频 sample 作为一个完整 AU 交付；H264 profile/reference/CABAC/
geometry 等硬件能力检查仍在 codec 层。转换背景可参阅
[FFmpeg 的 h264_mp4toannexb 文档](https://ffmpeg.org/ffmpeg-bitstream-filters.html#h264_005fmp4toannexb)。
裸 AAC AU 和 ASC 分别输出，不给 AAC payload 擅自加 ADTS 头。

## 已完成的主机验证

在仓库根目录运行：

```powershell
.venv/Scripts/python.exe scripts/test-player-mp4.py
```

默认使用 `tools/ffmpeg/bin/ffmpeg.exe` 及同目录 `ffprobe.exe`；可传 `--ffmpeg`
和 `--cc`。Windows 使用现有 MSVC 环境或通过 vswhere/vcvars64 初始化。
测试产物仅放在 `build/player-host/mp4`，不提交生成的媒体和解码输出。

25 项验证已经通过：

- 17 个真实 payload 样本：moov 头/尾、44.1 kHz stereo M4A、VFR、CTTS v0/v1、
  empty edit、co64、小型真实码流位于稀疏文件 4 GiB 之后、完全非交织 mdat、
  多 mdat、样本表跨页、AVCC 1/2 字节前缀、源 AUD/重复 SPS/PPS、v1 时间/编辑
  表、固定 stsz。每包 offset/size/PTS/时长与 FFprobe 对照；AAC payload
  拼接后逐字节一致，全部视频的 PC 解码逐帧 CRC 与原 MP4 一致。
- 回调每次仅返回七字节的 short-read 测试，规范化输出逐字节相同。C 测试也
  验证不足容量拒绝、64 字节 padding 和重复 EOF。
- 7 个负例：fragmented、错误 codec tag、2 倍 edit、复杂 montage、LC 外层
  携带 SBR 扩展、越界 NAL 长度、截断文件。

生产源另外通过现有 MIPS GCC 的 `-std=c11 -O2 -Wall -Wextra -Werror -EL
-mips32r2 -msoft-float -G0` 编译。稀疏 4 GiB 测试只证明解封装和主机 IO64，
不放宽实际 TF/FAT32 文件小于 4 GiB 的限制。尚未把本模块的主机测试当作
MP4 整机播放、横屏旋转速度或 H264 扩展 profile 的板端验收。

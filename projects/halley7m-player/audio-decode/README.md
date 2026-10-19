# MIPS 音频解码与 PCM 转换

本模块在唯一媒体任务内同步运行，不创建任务，也不访问 TF、UART、共享 PCM 环或音频硬件。父工程链接 `player_audio_decode`；它传递链接固定点 FAAD2 静态库 `player_faad_fixed`。输出仍由现有音频客户端交给所选 audio owner。

首批范围：MP3 Layer III，以及 AAC-LC 的 ADTS 帧或 MP4 裸 AU；44.1/48kHz、单/双声道已用真实编码文件验证。解码器接受 8–48kHz 的单/双声道，AAC 仅采用 1024 样本帧，拒绝 ASC 中明确声明的其他 object type、960 样本帧、多声道和超出范围的采样率。SBR/PS 编译关闭，HE-AAC 不在支持范围；ADTS 中后向兼容的隐式扩展可能仅解出 LC 核心，不能据此声称支持或完整识别 HE-AAC。会话中途改变采样率或声道数会失败。

## 固定源码与许可证

| 依赖 | 固定官方 revision | 本地配置 | 许可证 |
| --- | --- | --- | --- |
| [minimp3](https://github.com/lieff/minimp3) | `ea99364f61c14656440e8d77e9c233ccf3124633` | `MINIMP3_ONLY_MP3`、`MINIMP3_NO_SIMD`，内部浮点、输出 S16 | CC0-1.0 |
| [FAAD2 2.11.2](https://github.com/knik0/faad2/tree/2.11.2) | `673a22a3c7c33e96e2ff7aae7c4d2bc190dfbf92` | `FIXED_POINT`、`LC_ONLY_DECODER`、`DISABLE_SBR` | GPL-2.0-or-later，保留原 Nero 通知 |

源码在仓库 `third_party/player-minimp3` 与 `third_party/player-faad2`，保持上游字节未改；每个目录的 `SOURCE.json` 记录来源与逐文件 SHA256。FAAD2 原归档 SHA256 为 `98725cefc915771f00ffd0286901c865e7d3fd0e5ff6b98d004d6f48904776f9`。测试会校验这些摘要。上游 `LICENSE` / `COPYING`、README 与源文件版权头均随源码保留。包含 FAAD2 的软件分发应遵循其 GPL 条件；其 README 要求的版权消息为：

> Code from FAAD2 is copyright (c) Nero AG, www.nero.com

## 解码与裁剪合同

`player_audio_decoder_create()` 创建 opaque 对象；RAW AAC 配置中的 ASC 仅在创建时读取。`player_audio_decoder_decode()` 每次接收一个完整 MP3 帧或 AAC AU（ADTS 包含头），返回源采样率的 interleaved S16。`frames` 是每声道帧数，`consumed_bytes` 是输入字节数。返回的 PCM 指针到下一次 decode/destroy 前有效；转换器产生背压时，必须先消费该 PCM 再解下一包。错误后由会话销毁解码器，不尝试自动跨坏包恢复。

时长、edit list、编码器延迟和 padding 由上层处理，先在源样本轴裁剪，再送转换器。这里有两个实测必要区别：

- FAAD2 会吞掉第一帧 AAC-LC 的 1024 个源 PCM 帧，首个成功 decode 返回 `frames=0`、`decoder_skipped_frames=1024`。上层先抵扣这部分，再应用尚未消耗的 MP4 edit/priming，避免重复裁剪。此值来自所用解码器实际行为，不以相关搜索推断。
- minimp3 core 不解析 Xing/LAME，也不自动裁剪。上层先排除 Xing/Info 元数据整帧；按 [同 revision 的官方 minimp3_ex.h](https://github.com/lieff/minimp3/blob/ea99364f61c14656440e8d77e9c233ccf3124633/minimp3_ex.h#L177) 约定，LAME/Lavc 字段头裁为 `encoder_delay+529`，尾裁为 `encoder_padding-529`（仅正尾裁生效）。Xing frame count 计音频帧，不含元数据帧；排除元数据后不再减一次 1152。四个带标签真实文件已验证该规则精确恢复原源样本数。

所有正常返回都要求输入完整消费；`frames=0` 且完整消费是合法 priming。解析 ID3、划 MP3 packet、MP4 demux 以及时间戳不属于这个模块。

## 流式转为 48kHz 单声道

`player_pcm_convert_init()` 固定本次源格式；`process()` 的输入、consumed 均计源 PCM 帧，输出、produced 计 48kHz 单声道样本。每次只复制和消费能够处理的前缀，不保留调用方指针。双声道用 int32 算术平均（向零截断），再按精确有理相位 `k*source_rate/48000` 线性插值。当前使用线性插值，未提供带限重采样器的频率响应保证。

在 EOF 反复调用 `finish()`，容量不足时保留尾段，最终以最后一个源值补齐准确计数：`floor(trimmed_source_frames*48000/source_rate)`。重复完成的 finish 不输出额外样本。计数函数通过商与余数计算，并检查 uint64 溢出；24 小时计数及跨 AU/不规则分块结果已验证，无累计取整漂移。process 可在没有新输入时排出当前已能生成的输出；完成前都需保留未消费的输入前缀。

## 可复现验证与资源

在仓库根运行：

```powershell
python scripts/test-player-audio-decode.py
python scripts/test-player-audio-decode.py --mips-prefix E:/GitProject/X2600/toolchains/xburst2-windows/mips-ingenic-xburst2-elf-tools-x86_64-w64.r5.1.6/bin/mips-sde-elf-
```

脚本通过现有 `test-player-amp.py` 初始化 MSVC，用 CMake/Ninja 构建实际生产库，生成真实 FFmpeg MP3/AAC/M4A，再取真实 packet/AU 逐包解码。`--ffmpeg` 可指定可执行文件；默认取 `build/player-tools/dependencies.json`。不需要网络、串口或板卡。产物和 PCM 比较报告位于 `build/player-audio-decode-host-ninja`；MIPS 的 Debug/Release `-fstack-usage` 报告位于 `build/player-audio-decode-mips-stack.json`。

已通过：

- 16 个真实文件：44.1/48kHz × 单/双声道 × 无标签 MP3、ADTS、M4A 裸 AAC、Xing/LAME MP3。原生帧数、首帧跳过与显式裁剪均检查，没有对 PCM 做相关搜索、拟合增益或延迟修正。MP3 最大误差 1 LSB；AAC fixed 对 FFmpeg 浮点参考最差 RMS 15.092 LSB、峰值 288 LSB、SNR 49.148dB。测试合同为 SNR≥45dB、瞬态峰误差≤512/32768 全幅，报告保留实测误差。
- 9 个采样率的 mono/stereo 转换，空流、1/2 样本、短尾及长块，整块与不规则输入/输出容量逐样本一致；精确 EOF、重复 finish、溢出拒绝及 24 小时无漂移计数。
- 截断包，以及 RAW AAC 的 MAIN、96kHz、多声道、960 样本帧拒绝；逐文件 vendoring SHA256 校验。
- 真 MIPS GCC Debug/Release 静态库编译。Release 使用 `-O2`、MIPS32r2/ILP32 hard-float，未链接或运行 RV 音频硬件。

Release `.su` 的较大单函数栈：`mp3dec_decode_frame` 16688B；FAAD2 `channel_pair_element` 24600B、`single_lfe_channel_element` 22544B、`reconstruct_channel_pair` 8272B、`ifilter_bank` 8328B、`faad_imdct` 4176B。后几项存在嵌套调用，不能把最大单函数值当作整个任务峰值。现有 16KiB 媒体栈不足，集成采用 96KiB 初值，并仍需板端 stack high-water 确认。解码器对象和 FAAD2 通道状态用 libc 堆申请并在 destroy 释放；minimp3 临时 scratch 位于调用栈。

这些结果验证了解码和 PCM 转换软件。新格式的板端实时性能、实际发声、时钟与资源高水位由整机集成验证给出。

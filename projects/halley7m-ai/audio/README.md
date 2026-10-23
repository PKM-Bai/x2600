# Air1103 音频核心

本目录提供可嵌入的 C11 协议、降采样和 PTT 状态核心，目标库名为 `ai_audio_core`。
无动态内存、FreeRTOS、UART、网络和 LVGL 依赖。没有执行板级初始化，也没有模拟驱动成功。
X2600 的 UART 引脚、电平、DMA、2 Mbps 持续传输及实际尾音仍须单独实板验证。

## 协议接口

`ai_air1103_init/feed` 消费任意 UART 字节块，完成帧通过同步回调交付。
回调内的 payload 是临时借用，不得保存指针或重入解析器。
错误后逐字节重新寻找 `55 AA`，保留同批数据中的后续完整帧；不会像参考 Lua 一样
在第一次校验失败时丢掉整个接收缓冲。最大 payload 为 512 B，超过该上限记录长度错误。
已知 MIC 帧必须是 512 B，播放帧必须是 320 B；其他命令在上限内保持原始 payload。

协议依据为合宙官方 [air1103.lua](https://raw.githubusercontent.com/openLuat/LuatOS/master/script/libs/air1103.lua)
（读取于 2026-10-23，文件声明版本 1.3、日期 2026-09-20）：

- 帧为 `55 AA | payload长度BE16 | 命令BE16 | payload | SUM8`。
- 长度只计 payload，不含命令、包头或校验；SUM8 累加校验前所有字节。
- `2080` 上行 512 B/16 ms；`2081` 下行 320 B/10 ms，均为 16 kHz、16 bit、单声道。
- `0201` 是停止 MIC 上行，不能用它实现“停止播放后马上录音”。
- `0203` 音量值为 0..31；`0205 00` 为程序复位。

`ai_air1103_pack` 只组帧，不发送。驱动适配层负责持续消费 MIC，在非录音期丢弃，
以及下行每 10 ms 的发送节拍、欠载补零、尾帧补零和 DMA 所有权。
本模块保持 PCM 字节原样，上行实际端序须在实板阶段确认。

## 24→16 kHz 降采样

`ai_resampler_process` 接收宿主端序 `int16_t` 单声道样本，返回已消费和已生成的样本数。
输出缓冲不足时调用方继续传入未消费部分，不得跳过输入。
每个新响应先 `ai_resampler_init`；响应结束调用 `finish` 至返回 1，响应取消直接丢弃状态。
`finish` 对输入末端补零，最终恰好输出 `floor(N*2/3)` 个样本，包含极短输入。
内部只保留 64 个输入样本，24 个输入样本的前视相当于 1 ms 算法等待。

滤波器采用两个分数相位、每相位 48 taps 的 Blackman 窗低通，截止频率 7 kHz。
系数离线按以下公式生成并直接固化为 Q30，运行时只做整数运算：

```text
phase = 0 或 1，j = 0..47
d = j - 23 - phase/2
fc = 7000/24000
s = (d==0) ? 2*fc : sin(2*pi*fc*d)/(pi*d)
w = 0.42 - 0.5*cos(2*pi*j/47) + 0.08*cos(4*pi*j/47)
a[j] = s*w
q[j] = round(a[j]/sum(a) * 2^30)
q[23] += 2^30 - sum(q)
```

每相位系数和为 1，64 bit 累加、四舍五入和饱和输出。
验证包括 1 kHz 增益和 9/10 kHz 降采样后的混叠抑制，不能用“插值后样本数正确”替代滤波验证。

## PTT 适配契约

`ai_ptt` 由单一任务拥有，按下/释放输入须已经去抖；函数返回动作位，适配层执行动作。
状态为 `IDLE / STOPPING / RECORDING / WAITING / PLAYING`。
录音同时以 30,000 ms 和 480,000 个 16 kHz 样本封顶；`capture` 返回的 accepted 数量
必须先写入录音缓存，再处理同次返回的提交动作。提交时没有样本则丢弃，不生成空请求。

每次新按下或取消会推进非零 `generation`；所有云端事件先用
`ai_ptt_response_current` 检查代次，旧音频不得调用播放开始、结束或继续入队。
`STOP_PLAYBACK` 动作的处理顺序为：停止旧 PCM 排队与发送，清本地队列，然后以
**当前新代次** 调用 `ai_ptt_playback_stopped`，该确认才开始尾音等待计时。
旧代次确认和播放进行中的错误确认均不生效。

尾音等待没有默认已验证值，必须显式用 `ai_ptt_set_tail_delay` 传入实测值。
未知时返回 `NEED_STOP_TIMING` 并保持等待，不通过任意常数假装已经静音。
`tick` 在尾音等待结束且按键仍按住时产生 `START_CAPTURE`，30 秒从此时计时。
等待中提前松键不会生成录音；自动提交和取消后须等物理按键释放才能再次开启。
自然播放结束同样保留尾音等待，不能把“最后一包写入 UART”当成扬声器已经静音。
`response_end` 只结束没有本地播放的 WAITING 状态，音频响应需等本地播放完成后调用
`playback_end`。响应接收完成不等于播放完成。

## 主机验证

从仓库根目录运行本机已有 MSVC 生成器：

```powershell
cmake -S projects/halley7m-ai/audio -B build/ai-audio-msvc -G "Visual Studio 18 2026"
cmake --build build/ai-audio-msvc --config Release -- /m:1 /nr:false
ctest --test-dir build/ai-audio-msvc -C Release --output-on-failure
```

也支持标准 CMake/GCC 环境；主工程用 `add_subdirectory(audio)` 链接 `ai_audio_core`，
交叉构建保持 `AI_AUDIO_BUILD_TESTS=OFF`，并给静态库设置与固件一致的
`-mips32r2 -mabi=32 -mfp32 -mhard-float -EL -mno-abicalls -fno-pic -G0`；
父可执行目标的 `target_compile_options` 不会自动应用于静态库。宿主测试使用 CHECK 宏，Release 不会因
`NDEBUG` 跳过断言。测试不打开串口，也不能作为 Air/X2600 实际联调证据。

2026-10-23 验证记录：MSVC 19.50 Release 编译及 CTest `ai_audio_core` 通过。
实测 1 kHz 增益为 0.999967，9 kHz 输入的混叠幅度为 -74.59 dB，10 kHz 为 -79.82 dB；
同时通过全部协议拆分位置、粘包、错误重同步和 PTT 状态用例。
XBurst2 GNU 7.2.0 使用上述 MIPS 参数完成三个生产源文件及静态库交叉编译，未运行板测。
本机 MinGW 编译器因系统 DLL relocation 无法启动；MSBuild 首次并行构建未完成并已取消，
改用 `/m:1 /nr:false` 后实际编译与测试完成，未把中断前的运行状态记作通过。

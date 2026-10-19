# RAM 测量记录器

`player_measurement.c` 只由现有 console/media worker 调用，不创建任务、播放媒体或改变音量。主循环先调用 `player_diagnostics_poll()`，再调用 `player_measurement_step()`；记录器只读取非消费的 `player_diagnostics_latest()`。它不会打印周期日志，结束也只通过 `step()` 返回值通知调用者。

```c
int player_measurement_begin(const char *label, uint32_t duration_seconds);
int player_measurement_step(void);
int player_measurement_stop(void);
void player_measurement_get_info(player_measurement_info *out);
void player_measurement_print_summary(void);
int player_measurement_dump_csv(void (*pump)(void));
```

`begin()` 接受 1–1800 秒和 1–47 字节标签，成功为 0，正在记录为 `-EBUSY`，参数错误为 `-EINVAL`。`step()` 每至少一秒保存一条快照，新结束时返回 1，其余返回 0；worker 延迟时保存实际时间的一条记录，不补造遗漏的秒。`stop()` 新停止返回 1，已停止返回 0，必要时补一条带 FINAL 标志的末段。运行期间 `dump_csv()` 返回 `-EBUSY`，不输出任何字符。摘要和 CSV 仅在显式调用时输出；调用者先确认媒体 IDLE 且音频 inactive，再导出长 CSV，以免 UART 占用 worker 干扰音频。可选非 UART `pump` 在每条数据行前调用一次，生产接 `player_amp_service` 维持心跳；传 NULL 则不回调。

固定容量为 1860 条。真实 MIPS ILP32 构建每条 216 字节，记录数组 401760 字节，整个模块 BSS 438000 字节，其中会话快照为静态 35328 字节，未放在任务栈上。正常 1800 秒运行产生 1800 条；容量用尽、目标时间到达、首次观察到播放后 EOF/STOP/error 或显式 stop 都会结束记录。若在 IDLE 时先 begin，则允许随后人工启动播放。

CSV 的 `elapsed_ms` 来自 MIPS 单调时钟；音频位置和水位单位为 48kHz 单声道样本，队列为包数，CPU `busy_bp` 为百分之一百分点（10000 表示 100%）。每个 CPU 的 `counter_end`、`counter_hz`、`window_us` 保留原始窗口口径。只有新 sequence/结束计数、有效且至少一秒的 CPU 窗口计入均值，以实际窗口时长加权；begin 时已缓存的窗口仅作基线。两个核的窗口无需同相，未运行的 RISC-V 不记作 0% 负载。scheduler idle 口径和 ISR 归属遵从 `player_cpu_diagnostics.h`；摘要中的最小空闲堆是本次采到的 `heap_free` 最小值，CSV 另保留分配器/采样器的 `heap_min` 和 `heap_min_kind`。

媒体计数按 `session`、音频计数按 `audio_session`、通信计数按 `amp_session` 分段，分段内使用无符号差处理 uint32 回绕，分段切换从新计数累加。CSV 保留各时刻原始计数，摘要为 begin 后的差值累计。`max_late_us` 是观察到的会话累计峰值，若在会话中途 begin，可能包含测量开始前已经出现的峰值；它表示音频 completed 时钟下提交/丢弃决策的迟到，不是经过校准的声光延迟。

`shared_ring_samples` 是共享 PCM ring 的 write-read 差；`pending_valid_samples` 是客户端 written-completed，包含共享、私有缓冲及 FIFO 中全部尚未完成的有效样本。两者含义不同。视频包、音频包与待送 PCM 样本来自真实会话队列。水位为每秒观察到的峰值，不能证明采样间隙没有更高瞬时值。

`flags` 为以下位的按位或：FINAL=1、MIPS_FRESH=2、RISCV_FRESH=4、RISCV_RUNNING=8、SCOPE_CHANGED=16、COUNTER_WRAP=32、RING_INVALID=64、AUDIO_INVALID=128、SNAPSHOT_INVALID=256。无效水位不参与峰值；SNAPSHOT_INVALID 行沿用上次一致会话快照并显式标记。标签在 CSV 中使用标准双引号转义。

验证命令：`python scripts/test-player-measurement.py`。测试编译实际生产记录器，只替换时间、源快照和 UART 接收端，覆盖 1800 秒运行、缩小容量满表、CPU 缓存去重、计数/时间回绕、会话切换、EOF/error/STOP、末段和 CSV 转义，且断言采集时没有 UART 输出。另已使用实际 MIPS GCC 以 `-O2 -Wall -Wextra -Werror` 独立编译；板上长时间采集仍需集成后验证。

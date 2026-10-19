# 播放器板端日志分析

`analyze-player-board.py` 读取已有日志，不连接串口、不操作设备，也不改变固件或音量。它从混合 UART 文本中提取 measurement CSV、`MEASURE_SUMMARY`、`SESSION_STATUS` 和紧随状态的 `SESSION_QUEUES`，输出 JSON 与中文 Markdown。

```powershell
python scripts/analyze-player-board.py build/player-tf-avi-prefix-board.log `
  --label amp-prefix --window 600 --output build/player-board-analysis/prefix.json

python scripts/analyze-player-board.py --compare build/mips-run.log build/amp-run.log `
  --window 600 --output build/player-board-analysis/comparison.json
```

单日志 `--label` 选择日志中已存的 measurement 标签；不指定时逐标签报告。对照模式保留两个日志的标签，分别列出同一个 0..600 秒快照窗口，不要求同名。一个标签应只对应一次记录器运行。重复导出同一 CSV 会按时间和完整字段去重；同标签、同时间但内容不同会标记冲突，不能据此宣称完成验收。重新运行时使用不同标签，避免把复位前后的记录混为一次。

默认输出到 `build/player-board-analysis/`。`--output` 指定 JSON 路径，Markdown 使用相同文件名和 `.md` 后缀。日志不存在、没有 measurement 数据、指定标签不存在等输入错误返回非零。成功生成“证据不足”的报告仍返回零；验收状态由 JSON 字段给出，不能仅检查程序退出码。CSV 存在时，配对的 `SESSION_STATUS` 必须属于该 CSV 出现的 session；不同已结束 summary 或非末行 FINAL 都视为运行边界歧义。

## 指标口径

`elapsed_ms` 用于选择 0..`--window` 秒内的快照。CPU 只纳入 `valid`、FRESH 标志一致且窗口至少一秒的样本；RV 还必须带 RISCV_RUNNING。均值为 `sum(busy_bp * window_us) / sum(window_us)`，10000bp 等于 100%。无有效窗口返回 `null`，不把 RV 复位、缓存未更新或缺采样解释为 0% 负载。JSON 保留所用窗口的 `counter_end/counter_hz/window_us`，重复结束计数去重，重叠窗口或计数时钟复位显式报告。

CSV 没有导出 measure begin 的绝对时钟，两个核的计数时钟起点也不同。因此工具对同一 **快照范围** 纳入完整的真实 CPU 窗口，并列出累计有效窗口秒数；不会把 CPU 源窗口假定为该 CSV 行之前的一秒，或声称它们精确切在 0/600 秒边界。有效窗口秒数不足时不能用整段日志长度补零。这里的 CPU 是 scheduler idle 口径：ISR 归被打断任务，idle 中的 ISR 不单独计入 busy。

表中的堆最小值取本窗口有效 CPU 样本的 `heap_free` 最小值。CSV 的 `heap_min` 另按 `heap_min_kind` 保存：1 是分配器历史低水位，2 是采样器历史低水位，可能包含 begin 前的历史，不能与本次运行最低值混用。

计数按固件定义分组：

| scope | 计数 |
| --- | --- |
| `session` | elapsed samples、decoded/displayed/dropped/late、audio underruns |
| `audio_session` | audio written/completed、DMA errors/timeouts、queue/FIFO underruns |
| `amp_session` | 四组诊断/音频消息读写位置、rejected |

只对相邻、相同 scope 的记录做差。首条 CSV **不是 begin 基线**，工具不把其绝对值当作增量；`observed_delta` 不包含第一行前的变化。scope 改变时不跨段相减。相同 scope 下降且带 COUNTER_WRAP 时，只有小幅 modulo32 增量才作为回绕；无法解释的大幅下降标记 `reset_candidates`，不凭空加上 2^32。固件 summary 是包含 begin 基线的累计值，原样保留在 `source_summaries`，不混入 CSV 指定窗口重算。

水位单位为 PCM samples 或包，报告仅是每秒观察到的 min/max，不能证明采样间隙的瞬时峰值。RING_INVALID、AUDIO_INVALID、SNAPSHOT_INVALID 以及所有 flags 均有独立计数；相应无效观测不参与水位。负的 session/audio 错误也保留 min、max 和非零观测次数，不会因最大值为零而隐藏。

## 完整性与完成证据

工具检查重复行、同时间冲突、字段截断、摘要声明行数与实际唯一行数、FINAL 末行、超过 1.5 秒的采样间隔及窗口终点覆盖。长间隔可能来自 worker 延迟，也可能来自 CSV 缺行；工具报告间隔，不补造记录。只有连续的采样覆盖、完整末行与一致的行数，`sampling.csv_window_complete` 才为真。这个字段评价日志覆盖，不单独等于连续播放、性能或声光验收通过；另看 scope、状态、错误和完成证据。

`completion.internal_30min=evidenced` 要求真实 AVI ENDED 的 **86400000/86400000 samples、decoded=54000、displayed+dropped=54000**，且有音频/协议零错误证据：完整 ENDED CSV 错误字段，或同一日志中的匹配 `SESSION_STATUS` 与完整音频零错误 EOF summary。单独 `reason=eof`、`target_s=1800`、日志墙钟到 1800 秒或文件名含 30min 都不成立。CSV 冲突、截断和疑似计数复位会阻止工具给出该结论。1800 秒结束证据与前 600 秒 CPU 统计分开评价；完整媒体的尾部一行可以证明板内完成，但不能补全缺少的 600 秒采样。错误审核使用完整 run，600 秒之后出现的音频/协议错误也不能被干净的前 600 秒掩盖。

用户的视频标准是无持续性掉帧，并非绝对零。`completion.video_acceptance` 单独报告：观测到任何 drop/late 为 `needs-review`，全部已有观测为零则为 `no-drop-late-observed`，数据不足为 `not-evaluated`。少量 drop/late 不会推翻内部播放完成，也不自动等于视频质量通过。JSON 的 `video.sample_interval_deltas` 保留完整运行的相邻采样时间、scope、实际 drop/late 差分；跨 scope、无效快照或疑似复位返回未知，不补造“一秒”。Markdown 列出有变化或未知的区间，供人工判断出现位置和是否持续。工具不发明持续性阈值。

当前 `player-tf-avi-prefix-board.log` 只有 36.028 秒固件摘要，含暂停/恢复，累计 samples=948480、decoded=594、displayed=593、drop/late=0。它是功能短跑记录，不能算 10 分钟或 30 分钟验收；无 CSV 时也不重算 CPU 窗口。

实际 `player-prefix-measurement-trace-board.log` 的 36 条 CSV 已跑过：MIPS 加权 30.897%、RV 加权 1.329%，有效源窗口分别累计 36.202/36.322 秒；与固件整数摘要 3089/132bp 一致。观测最低空闲堆为 31681936/127648 bytes。工具识别出 session 2→3、开始的 IDLE 与暂停状态，不会把这个短功能片段认作 600 秒性能对照。其输出为 `build/player-board-analysis/prefix-dump.json/.md`。

所有报告固定保留 `physical_sync_100ms=not-evaluated`。板端样本与帧计数不能替代手机实拍、采集偏差校准和 ≤100ms 声光同步测量；实拍工具见 [PLAYER_SYNC.md](PLAYER_SYNC.md)。

## 验证

```powershell
python scripts/test-player-board.py
```

18 项回归直接导入生产解析器，覆盖 CPU 真时长加权、无效/未运行 RV、重复/重叠 CPU 窗、带引号标签、混合串口前缀、重复/冲突 CSV、scope 切换、小幅 uint32 回绕与疑似复位、截断/缺行、窗口外数据隔离、无效水位与负错误、虚假 EOF、完整 ENDED 计数、600 秒以后的错误、无关 session 的 ENDED、非末 FINAL/冲突摘要及 CLI 对照。另覆盖非零 drop/late 的帧守恒、逐区间事实与独立视频复核状态，以及非零 DMA 错误仍阻止完成证明。测试不会连接 COM 或修改播放器源码。

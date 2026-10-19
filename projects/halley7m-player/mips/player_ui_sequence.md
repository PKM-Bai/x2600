GUI 对照验证序列只通过原有 5ms 按键采样回调生成额外 stable mask，不代表物理按键板测。调用者用短临界区串行化 console 的 start/stop/status 与按钮回调的 sample，再把返回掩码 OR 到实际物理键，送入原有 `player_gui_sample_keys`。

启动后前 30 秒输出 0，此后每 120 秒执行一轮：Start 850ms → 释放 150ms → 菜单 B 100ms → 释放 250ms → Start 100ms → 释放 300ms → 诊断页 B 450ms → 释放 200ms → A 450ms → 释放 200ms → Start 100ms → 释放 300ms。A 在诊断页向上滚动，最后的短 Start 返回播放页；正常序列不在播放页生成 A/B 或短 Start，不调整 7% 音量、不暂停音频。600 秒完成 5 轮，1800 秒完成 15 轮。

每次注入前检查预期 page/menu/selection；真实按键、GUI 错误、错误页面/菜单或超过 100ms 的采样间隔立即终止并输出 0，不补发过去的动作。任何 `aborted=1` 都使本次同序列对照证据无效。取消或 stop 若发生于 Start 持有不足 800ms，释放可能被真实识别器解释为短按，`cancel_may_emit_short=1` 明确标记此边界，可能暂停播放或确认当前菜单；它不是无副作用的取消。正式 600/1800 秒终点处于 0 键等待区间，stop 不应插在执行中的周期。

`python scripts/test-player-ui-sequence.py` 将生产序列模块接入真实 `player_input.c`，验证正常动作/连发、时钟回绕及终止边界。测试使用 GUI 状态模型，实际页面渲染、按键 GPIO 和两种 AudioOwner 下的负载仍需板测。

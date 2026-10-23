# 回放输入来源

`deepseek-recorded-shape.sse` 使用 2026-10-23 成功单轮 PC 调用的脱敏事件记录：

- 原始运行目录：`build/ai-deepseek-probe/20261023-160645-4de418/`。
- 可随仓库保留的真实报告：`../../cloud-probe/fixtures/deepseek-live-single-20261023.report.json`。
- 输入记录：该运行中的 `events.redacted.jsonl`，仅提取 `direction=receive` 的事件。

保留了服务事件次序、空增量/文本增量/结束增量结构、`finish_reason=stop`、末事件中的实际 usage（29 prompt + 3 completion = 32）以及 `[DONE]`。已脱敏文本不能还原，因此五个 content 依次替换为 `""`、`"协议"`、`"回放"`、`"好"`、`""`；响应 ID 和 fingerprint 也已换成固定占位值，然后以 SSE `data:` 行重新序列化。

这是**真实事件结构配合合成文本的测试 fixture**，不声称是未修改的网络抓包，不证明 C 固件实际访问了云端。测试逐字节输入此文件，校验文本拼接、JSON 字段结构、结束条件与用量，防止只用自拟的简单 JSON 通过测试。

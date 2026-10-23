# DeepSeek 纯 C 协议层

本目录实现设备云端文字接口的请求编码与 SSE 响应解析，复用 `../core/ai_history.h`。主机测试及 XBurst2 MIPS 静态库编译已通过；`AI_DEEPSEEK_TRANSPORT_IMPLEMENTED` 为 `0`，尚未接入设备上的 HTTP、TLS、socket、网络任务或 LVGL。此阶段的编译通过不代表设备已经能联网问答。

固定协议来自 [DeepSeek 官方 Chat Completion 文档](https://api-docs.deepseek.com/api/create-chat-completion/)，并以 2026-10-23 已成功的 PC 请求事件结构校验：

- HTTPS 主机 `api.deepseek.com`，路径 `/chat/completions`，模型 `deepseek-flash`。
- `thinking.type=disabled`，`max_tokens=2048`，`stream=true`，`stream_options.include_usage=true`。
- 每次请求发送 system、最近 8 个完整 user/assistant 对、当前 user。历史只读取 `ai_history_context()`，失败、取消和截断回合不进入模型上下文。
- 当前服务把 `usage` 放在带 `finish_reason` 的最后一个内容事件中；解析器也兼容 `choices=[]` 的独立 usage 事件。

## 接入契约

1. 应用调用 `ai_history_begin()` 或 `ai_history_retry()` 获得本次 `active_id`。这个 ID 同时是协议的 `generation`，必须随本次网络操作保留；不得将旧连接的回调改标为新的 ID。
2. 调用 `ai_deepseek_write_request(history, generation, NULL, NULL, &length)` 验证 UTF-8 并测量 HTTP `Content-Length`；再以 writer callback 写出同一份历史。测量到发送完之间保持历史稳定。编码器分块输出 JSON，不分配完整请求缓冲。writer 必须在返回前消费或复制全部片段；返回零表示全部接收，返回非零会中止编码，由调用者关闭本次请求。
3. HTTP 层自行设置认证头、方法、内容类型与长度，并验证 HTTPS 证书。协议层不接收、不读取、不记录 API key。
4. 只有 HTTP `200` 且内容类型为 `text/event-stream` 后，才将已完成 HTTP transfer/content 解码的响应体交给 `ai_deepseek_stream_feed()`。HTTP chunk framing 不能直接传入。每次 feed 可以任意拆分 SSE 行和 UTF-8 字节。
5. `AI_DS_TEXT` 事件中的 UTF-8 片段只在 callback 期间有效。调用者可同步复制到 `ai_history_append()` 或带代次的工作队列；协议层本身不修改历史、不调用 UI。TEXT callback 返回非零会把本次请求置为 FAILED。
6. `AI_DS_FINISHED` 只通知一次。调用者依据状态调用 `ai_history_finish()`；仅 `AI_DS_COMPLETE` 对应 `AI_TURN_COMPLETE`。该通知的返回值不影响已确定的结果，也不会产生第二次通知。callback 不能重入同一 stream。
7. 用户取消时先调用 `ai_deepseek_stream_cancel()`，随后由网络层关闭请求。断流、超时或 HTTP 响应体结束时调用 `ai_deepseek_stream_end()`。解析失败或容量超限后也由网络层停止接收。没有自动重发；手动重试必须使用新的 generation。

`stream_feed()` 返回 `0` 说明本次字节处理成功，完成结果仍需检查 FINISHED 事件或 `stream.status`。`AI_DS_IGNORED` 表示旧代次或已经结束的请求；负数表示编码、解析或 callback 错误。所有接口都要求调用者串行访问同一 stream。

## 完整性与用量

必须同时收到非空文本、`finish_reason="stop"` 和 SSE `[DONE]` 才完成回合。收到 stop 后继续等待 `[DONE]`，因此可以接收迟到的用量事件；在 `[DONE]` 之前断流则失败。`length` 对应 LIMIT，`aborted` 等其他结束原因对应 FAILED；已经显示的残片由历史层按相应失败状态保留，不能加入后续模型上下文。

`ai_deepseek_usage.present` 按位表示字段是否已知，`ai_deepseek_usage_complete()` 要求 prompt/completion/total 三项均存在。缺少用量不使完整文本失效，也不推断成零；截断或失败时已经收到的用量同样保留。`stream.finish_reason` 可用于解释非正常结束，`stream.error` 用于本地协议错误。

同一代次内若服务响应 ID 改变、完成标记之后又产生非空内容、JSON/UTF-8 非法、事件过大或总答案超过 32768 UTF-8 字节，本次请求停止。超长答案不作为完整历史提交。初版只支持本请求所需的单个 choice、文字增量与用量；未请求 tools、reasoning 输出或其他模态。

## 内存与任务栈

`ai_deepseek_stream` 由调用者分配，MSVC x64 下为 **164240 字节**，约 160.4 KiB；应放入静态存储或协议任务持有的长期内存，不要放到 FreeRTOS 任务栈。它有各 64 KiB 的 SSE 行/事件缓冲和 32 KiB 的 UTF-8 解码缓冲，不在堆上分配内存。

JSON token 工作区固定 256 项。XBurst2 GCC 7.2 Release 的 `-fstack-usage` 实测 `parse_event` 自身栈帧 5240 字节、`stream_feed` 104 字节，另有 callback/调用链开销。**协议任务至少预留 8 KiB 栈，并在接入 TLS/网络 callback 后重新测量高水位**；这里的数字不能当作整条 TLS 调用栈上限。请求编码支持大量转义字符，最坏请求可超过 1.7 MB，HTTP 层应按 callback 流式发送而不是复制到固定小缓冲。

## 验证与依赖

`vendor/jsmn` 固定为 jsmn v1.1.0，MIT 许可证；原始头文件、许可证、来源与 SHA256 一起保留，没有改动上游头文件。

主机 CTest 的一个测试可执行文件覆盖 11 组场景：8 轮历史/JSON 转义、最大转义请求流式写出、所有单次切分点及逐字节分片、迟到 DONE/缺用量、SSE 多行与 Unicode、截断/断流/服务错误、取消与旧代次、非法 JSON/Unicode、容量上限、callback/历史集成、已验证事件结构回放。回放文件及真实证据关系见 [tests/README.md](tests/README.md)。

2026-10-23 实际执行结果：

- MSVC 19.50 + Ninja，`build/ai-cloud-msvc`：11 组通过，`sizeof(stream)=164240`。
- XBurst2 GCC 7.2，`build/ai-cloud-mips`：`libai_cloud_protocol.a` 及其历史依赖编译通过；协议对象 `.text=11170`、`.data=0`、`.bss=0` 字节（Release、未链接固件）。
- 本阶段没有访问真实 API 或凭据；真实 PC 请求的成功记录沿用 `../cloud-probe/fixtures/deepseek-live-*.report.json`。设备网络、TLS 与端到端调用仍待接入和验收。

在已配置 C11 编译器的 shell 中独立构建：

```text
cmake -S projects/halley7m-ai/cloud -B build/ai-cloud-host -G Ninja
cmake --build build/ai-cloud-host
ctest --test-dir build/ai-cloud-host --output-on-failure
```

Windows 使用 MSVC 开发环境；仓库 `scripts/test-player-amp.py` 的 `msvc_environment()` 可加载该环境。作为子目录接入主工程时，设置 `AI_CLOUD_BUILD_TESTS=ON` 才生成主机测试；MIPS 固件构建应设为 OFF，并使用现有 `cmake/xburst2-elf.cmake` 工具链。根工程的接线由集成阶段负责。

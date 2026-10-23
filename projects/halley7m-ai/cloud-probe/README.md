# PC 云端协议探针

这里验证云端接口和协议状态，不代表 X2600M、SWT6621-SH、以太网或 Air1103 已联调通过。

当前先使用用户已有的 DeepSeek API 验证文字交流；百炼原生 WSS 探针保留用于后续语音验证。两个探针都只将完整成功问答加入最近 8 轮历史，不自动重发失败请求。测试报告默认写入被 Git 忽略的 `build/`。

## DeepSeek 文字验证

仅依赖 Python 3.10+ 标准库。固定请求 `https://api.deepseek.com/chat/completions`，模型 `deepseek-flash`，`thinking.type=disabled`，`max_tokens=2048`，启用 SSE 和 usage。

在仓库根目录执行离线测试：

```powershell
python scripts/ai-deepseek-probe.py selftest
```

真实调用从 `DEEPSEEK_API_KEY` 环境变量读取密钥，或使用仅保存在本机、已被 Git 忽略的文件。`--key-file` 支持以下三种单行形式，标签不区分大小写；冒号也支持中文冒号。不要把真实密钥写入命令参数、测试 fixture 或提交。

```text
sk-<本机密钥>
DEEPSEEK_API_KEY=sk-<本机密钥>
API key: sk-<本机密钥>
```

```powershell
python scripts/ai-deepseek-probe.py text --text '请只回复：连接成功。' --key-file configs/config.txt
python scripts/ai-deepseek-probe.py scenario12 --key-file configs/config.txt
```

`--show-text` 可在终端查看测试回答，文件日志仍脱敏。默认终端只输出协议状态、耗时和 token 用量。没有凭据时返回退出码 2，并且不联网。

每次请求的消息是 `system + 最近8轮 user/assistant + 当前user`。12 轮场景检查跨轮标记回忆、第 10 轮只携带第 2～9 轮、第 12 轮新会话历史为空。清空上下文依据实际请求历史检查，不依赖模型逐字复述某句确认词。该场景以短问答测试连通和状态，不能代表长回答、全网环境或设备上的延迟。

解析器同时支持当前官方的“末内容块含 usage 和 finish_reason”以及旧式 usage-only 块。必须同时收到 `finish_reason=stop` 和 `[DONE]`，且存在回答文本，才将该轮记入历史。`length`、`aborted`、缺失 `[DONE]` 或传输异常均保留已收到的文本，但不记为完成。usage 缺失记为 unknown，不算成零，不为补取 usage 重发请求。

也支持自定义场景：

```json
{
  "turns": [
    {"text": "记住标记：蓝色纸船。"},
    {"text": "刚才的标记是什么？", "expect_substring": "蓝色纸船"},
    {"text": "你好", "new_conversation": true, "expect_history_turns": []}
  ]
}
```

```powershell
python scripts/ai-deepseek-probe.py scenario --scenario build/my-scenario.json --key-file configs/config.txt
```

## 百炼文字与语音验证

百炼需要另行具备北京地域的模型权限、API Key 和业务空间 ID；当前没有用真实百炼凭据验证。

```powershell
python -m pip install -r projects/halley7m-ai/cloud-probe/requirements.txt
python scripts/ai-cloud-probe.py selftest
```

真实调用读取 `DASHSCOPE_API_KEY` 和 `BAILIAN_WORKSPACE_ID` 环境变量；固定使用北京空间域名及 `qwen3.8-omni-flash-realtime`。需要由本机环境配置这些值，不能把真实值复制进仓库说明。

```powershell
python scripts/ai-cloud-probe.py text --text '你好，请简短介绍自己。'
python scripts/ai-cloud-probe.py audio --pcm build/input-16k-mono-s16le.pcm
python scripts/ai-cloud-probe.py scenario --scenario projects/halley7m-ai/cloud-probe/fixtures/bailian-mixed12.example.json
```

输入必须是 16 kHz、单声道、s16le 裸 PCM，无 WAV 文件头，非空且不超过 30 秒。原始 PCM 文件本身无法声明采样率，准备者须确认格式。示例混合场景引用的 6 段 PCM 需自行录制后放入场景文件旁的 `audio/`；不要将个人录音提交到仓库。

百炼每轮重新建立 WSS，并等待 `session.created`、`session.updated`。最近 8 轮完整文字放入 `instructions` 中的 JSON 历史区，当前文字或音频另行提交。文字轮次验证 `max_tokens=2048` 的回显；语音轮次目标回答约 200 字。音频 append 每包 100 ms，commit ACK 到达后才发送 `response.create`。输入转录允许在响应结束后最多迟到 5 秒；缺失或失败时不编造文字历史。

输出按 24 kHz PCM 保存为 WAV，最多保留 60 秒音频；到上限取消仍在生成的响应。这里验证云端数据，未实现设备 24→16 kHz 重采样和 Air1103 播放，也未验证实际喇叭停止尾音。

故障注入参数 `--cancel-after-ms`、`--disconnect-after-ms` 从 `response.create` 起计时；取消只在收到 `response.created` 后发送。单轮命令默认预期正常完成，因此主动中断的命令会以退出码 1 表明未正常完成。在自定义场景中可以用 `expect_status: "interrupted"` 或 `"injected_disconnect"` 验证预期异常。

```json
{
  "turns": [
    {"text": "请详细介绍中国历史。", "cancel_after_ms": 0, "expect_status": "interrupted"},
    {"text": "你好", "disconnect_after_ms": 0, "expect_status": "injected_disconnect"}
  ]
}
```

## 证据与门禁

每次真实运行输出 `report.json` 和 `events.redacted.jsonl`。百炼语音另输出 `turn-NN.wav`；WAV 包含生成的真实语音，不能称为脱敏音频。JSONL 中的输入文本、历史、转录和音频 payload 只保留长度与哈希；不会记录 Authorization 头。缺失计量轮次单独统计。`first_text_ms` 包含 DeepSeek 请求的建连耗时；百炼文字同样包含冷建连，百炼语音的首文本/首音频计时从文件播放完并提交输入开始。百炼另记录 `connect_ms`，不能把 PC 冷建连测试等同于设备空闲预连接效果。

2026-10-23 已完成 DeepSeek 真实单轮及 12 轮验证，摘要见 `VALIDATION.md`，已脱敏的真实报告见 `fixtures/`。百炼只完成离线 fake transport 测试，`cloud_g0` 不由工具自动宣布通过。DeepSeek 文字通过也不替代百炼文字/语音切换、转录、打断和 8 轮文字历史背景的真实验收。

后续公共历史对象保持 `[{turn, user, assistant}]`，只保存完整成功轮次；显示历史和失败/截断状态由设备 UI 另行管理。DeepSeek 转成角色消息，百炼转成 instructions 背景。模拟 fixture、离线单元测试与真实云端报告须明确区分。

官方协议依据（2026-10-23 核查）：

- [DeepSeek 首次调用](https://api-docs.deepseek.com/)
- [DeepSeek Chat Completions 参数和 SSE](https://api-docs.deepseek.com/api/create-chat-completion/)
- [百炼 Qwen-Omni-Realtime](https://help.aliyun.com/zh/model-studio/realtime)
- [百炼客户端事件](https://help.aliyun.com/zh/model-studio/client-events)
- [百炼服务端事件](https://help.aliyun.com/zh/model-studio/server-events)

# 正式板测运行器

`run-player-board.py` 控制已经运行正式播放器固件的 UART，不负责加载固件、重启、
NOR 写入或 TF benchmark。执行前独占串口，确认 TF 完整片源存在；另行完成
`tf bench`。脚本固定使用当前批准的 7% 音量作为前置条件，绝不发送 volume、tone
或自动修改初始 GUI 页面。播放开始三秒后必须是 PLAYING、播放页、无菜单和错误。

```powershell
python scripts/run-player-board.py --port COM52 --media 0:/multimedia/video/acceptance-30min.avi --seconds 1800 --label amp30 --log build/player-amp30-formal.log
python scripts/run-player-board.py --port COM52 --media 0:/multimedia/video/acceptance-30min.avi --seconds 600 --label mips10 --log build/player-mips10-formal.log
```

默认 115200 baud，可用 `--baud` 指定。日志必须是新文件，存在则拒绝运行，避免把
多个 run 拼成一个验收结果；label 为 1–47 个 ASCII 字母、数字、`_` 或 `-`。
MIPS/AMP 对比使用相同片源、时长、7% 音量与操作序列，分别运行和保存日志。

顺序为 stop、停止旧 UI sequence、确认媒体 IDLE/音频 inactive，再 play；三秒后
查询 session/audio/gui。验证后依次 `measure begin`、`ui sequence start`。
播放期间仅每60秒查询 session 与 UI sequence 状态，其余只接收日志，不周期输出
CPU 摘要。stdout 提供稀疏 JSON 进度，UART 原始行完整保留，主机 UTC/elapsed
放在独立 `[host ...]` 行，CSV header/data 不加前缀。

600秒运行要求测量自然 duration 完成；1800秒允许媒体由于三秒预热比测量/UI序列
提前约三秒 EOF。1800秒结束必须保存完整 ENDED decoded=displayed+dropped=54000、
86400000 audio samples、零 underrun/error 和完整15次 UI cycle 的证据；
600秒为5次 cycle。最终先采集并验证短 session/audio/gui/UI，保存 ENDED
证据，再停止 UI sequence、stop媒体、确认 IDLE/音频 inactive，之后才查询长
measure summary、验证已冻结的测量记录并导出 CSV。异常清理同样先确认空闲，
再打印 measure stop 摘要，避免长UART占用仍PLAYING的worker。
固件导出必须具备每行 AMP heartbeat pump。脚本核对返回码、记录数与唯一末 FINAL。

用户不要在正式 run 期间按实体键；任一实体按键使 UI sequence aborted，该 run
必须无效。命令拒绝、异常状态、音频或 GUI 错误、超时/串口错误同样
返回非零，并尝试停止 sequence、媒体和测量后确认空闲。断线时只能记录清理
未确认，不重开串口、不重播或重启。失败日志应保留，不以新的尝试覆盖。

drop/late 保留实际数字，不因一次掉帧或迟帧提前中止长跑。出现时最终
`video_acceptance=needs-review`，交由窗口/连续性分析判断是否持续性掉帧。
`complete valid=true collection_complete=true` 只表示内部状态与记录流程完整，
不自动代表720p30通过，结果仍需 `analyze-player-board.py` 审核。用户已取消手机录像及
实际声光偏差数值验证，该项不再阻塞交付；尚未实测的偏差不记为≤100ms已通过，
保留的手机分析工具仅作可选诊断。

完成事件分别保留测量窗口 drop/late、STOP前状态和STOP后状态。边界操作之后的
掉帧属于窗口外观测，不能回填到已冻结CSV或被隐去。首个正式MIPS600测量为
600005ms、decoded/displayed增量18001/18001、窗口drop/late为0；旧顺序在仍播放时
打印长摘要后，STOP状态另见1帧窗口外掉帧。原始记录保留，600秒窗口仍有效。

命令完成以完整响应换行判定，session 还需完整 SESSION_QUEUES 第二行；不等待
尾部 `player> `。真实 newlib stdout 可将没有换行的 prompt 缓存到下一次命令
回显时才发送，不能把 prompt 当作响应结束保证。

无硬件回归命令：`python scripts/test-player-runner.py`。Fake UART 按真实行缓冲方式
分块发送响应，虚拟时间运行600/1800秒，覆盖提前EOF、拒绝、前置条件、实体键/GUI中止、
串口断开、超时、截断CSV、异常停止确认与长摘要导致的窗口外掉帧。另将首次失败日志中的 stop/UI/session/audio
实际响应保存为 `tests/player_runner_uart_fixture.json`，逐字节回放且不提供尾prompt。
测试不会 import/open 真串口。

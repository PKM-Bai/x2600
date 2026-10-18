# X2600M ReGBA：原生存档与音频实施记录

> 本文保留各阶段当时的实现与验收范围；当前工程状态见[项目 README](../projects/halley7m-regba/README.md)。`build/` 原始日志、镜像与备份，以及 `Halley7/` 原始硬件资料和本机工具包均在开发机保留，不随源码提交；精选摘要在 `docs/validation/`，其中保留的本地路径用于追溯。

更新日期：2026-10-18。本轮完成游戏原生 `.sav` 保存、重载与内置音频连续性修订。三类诊断存档往返和普通复位后恢复已通过；H 在真实 `test4.gba` 上通过 36 观察点静音生命周期、游戏 / LCD / DMA 运行时完整 ROM CRC、手动原生保存及 TF 读回，并完成带屏、非零 PCM 与 TF 的 30 分钟联合压力：主机 1800.188 秒，音频四项错误与 LCD 欠载均为 0。独立核对全日志 35 份完整状态，保存 1 / 1 无错误、无待提交修改；正式长测没有新增原生保存提交，实际写卡由测前独立用例验证。用户确认长测期间画面正常、无明显异常。真实游戏新的保存点恢复和声音听感仍未专项确认。今日复用 H，没有新代码或构建；所有板上 ReGBA 验证保持静音，不播放测试音，不写 NOR。

## 原生存档的行为

本轮保存 GBA 游戏自身的 SRAM / Flash / EEPROM 数据，与游戏内“保存进度”对应，不包含 CPU、画面和全部模拟器状态，因此不是即时存档。文件路径由 ROM 文件名生成，例如 `/gba_game/test4.gba` 对应 `/GBA/SAVES/test4.sav`。

| 类型 | 支持的 `.sav` 长度 |
|---|---|
| EEPROM | 512、8192 字节 |
| SRAM | 32768 字节；核心也支持 65536 字节变体 |
| Flash | 65536、131072 字节 |

加载时先填充整个备用 RAM 为 `0xFF`，然后读取合法长度的存档并设置对应类型和容量。65536 字节文件可能属于 SRAM 或 Flash，本轮改为默认按 SRAM 接受直接写入，同时保留 SRAM / Flash 的 64KiB 容量；真正的 Flash 游戏发出解锁命令时切换为 Flash，再执行相关操作。这样既保留 Flash 协议识别，也避免 64KiB SRAM 重载后直接写入被忽略。没有存档时按新游戏启动；长度非法、读取不完整或关闭失败时明确报错并取消此次 ROM 加载，避免把坏档当成空白进度继续覆盖。

修复了两个与自动保存有关的上游行为：`load_game_config()` 不再无条件清空先前由存档识别的类型，显式兼容配置仍可覆盖；SRAM 地址 `0x5555` 的专用写分支补上修改标记，避免仅更新该字节时漏掉保存。

### 保存时机与后台写入

核心在帧边界消费客体的修改标记。最后一次修改后连续 2 秒没有新修改时，复制当前数据到独占快照，交给后台任务写 TF。运行中执行 `save` 会请求在帧边界立即提交快照；该命令被接收不等于 TF 写入已经完成，需要结合后续 `SAVE complete result=0` 和状态计数确认。

停止核心时等待正在进行的保存，并提交尚未写出的修改。切换 ROM 前也执行同样的 flush；失败时保留旧 ROM 及 RAM 中的数据，取消切换并提示重试。后台写入失败重新标记为待保存，采用 2 秒间隔重试；保存期间又发生的游戏修改仍保持待保存状态。

`platform/battery_save.c` 使用一个最大 128KiB 的快照，任务优先级 2，栈 2048 words / 8KiB。快照复制期间不长时间关闭中断；低优先级后台任务执行文件操作，FatFs 仍由原有 filesystem 任务单独访问。快照正在使用时返回 `-EBUSY`，不会覆盖尚未完成的提交。实际写入前准备 `/GBA/SAVES` 等资源目录；仅启动或挂载 TF 仍不会自动创建目录。

保存顺序如下：

1. 将快照写到 `game.sav.tmp`，逐项检查 `fwrite`、`fflush`、`fclose`；底层 `f_close` 执行 FatFs 同步。
2. 写入和关闭均成功后，移除更早的 `game.sav.bak`，把当前正式文件改名为 `game.sav.bak`。
3. 把已写完的 `.tmp` 改名为正式 `.sav`；失败时尝试将 `.bak` 回滚为正式文件。
4. 加载时若正式 `.sav` 缺失而 `.bak` 存在，则恢复 `.bak`。已有正式文件优先，残留 `.tmp` 不作为完成的存档读取。

`.bak` 是上一次完成的版本，恢复时进度可能比最近一次提交旧。2 秒等待、后台排队及写卡期间存在尚未完成的窗口，不能据此承诺任意断电都保留最新进度；本轮也没有进行写入中断电试验。

### 存档诊断

`status` 新增两组信息：

```text
backup dirty=0 in_flight=0 requested=0 autosave_idle_ms=2000
SAVE queued=0 completed=0 errors=0 busy=0 bytes=0 crc32=00000000 last_result=0 path=-
```

上面只是初始格式示例，不是本轮实板结果。`dirty` 表示游戏 RAM 有待提交修改；`in_flight` 表示核心等待后台结果；`requested` 表示显式保存请求。`queued` 为已接收快照数，`completed` 统计所有完成尝试，失败另加到 `errors`；`last_result=0` 表示最近完成尝试成功，负值为 errno。`crc32` 对应快照内容，不能单独替代写卡后读回校验。

后台每完成一次尝试输出一条 `SAVE complete` 摘要；不在逐帧修改路径打印日志。`.sav` 成功保存后，重新加载 ROM 会输出 `SAVE loaded path=... bytes=...`。

## 内置音频修订

2026-10-16 三键版的静音实板运行出现队列和 FIFO 欠载各一次，后续约 50 秒未继续增长；该旧版本现象见[板载按键记录](ReGBA阶段6板载按键实施记录.md)。本轮针对已发现的竞争条件和可测的调度干扰修订，各版本实际结果分别记在后文；H 的真实游戏静音生命周期和 30 分钟联合压力已取得零欠载证据。

- 修复 PCM 入队与异步停止之间的竞争。停止后的环形队列重置不会被中途恢复的旧提交污染。
- 用停止代数保留启动等待期间的取消请求，包括 codec 约 300ms 稳定等待；取消后的启动不会重新打开输出。
- 输出启用推迟到首个 DMA 周期已成功启动之后。初始化和默认启动保持 DAC 静音、PE17 功放关闭。
- 移除游戏运行期间每 60 帧的栈诊断打印，保留启动、停止和显式状态查询，减少 UART 阻塞对 PCM 生产的干扰。
- 增加完整周期发布和 DMA 重装的时间诊断，便于区分生产间隔与服务延迟。

`status` 中的 `audio_timing` 包含：

| 字段 | 含义 |
|---|---|
| `max_ready_gap_us` | 完整 PCM 周期发布间隔的最大软件观测值 |
| `underrun_ready_gap_us` | 最近一次队列欠载距最近完整周期发布的间隔；后续欠载会更新 |
| `max_dma_restart_us` | DMA 完成处理到重新启动之间的最大软件观测耗时 |
| `first_queue_underrun_us` | 本次启动首次队列欠载的板端单调时间；未发生则为 0 |

这些值按软件观察点记录，不是 FIFO 硬件实际变空时刻的测量。原有 `queue_under`、`FIFO_under`、`errors`、`timeouts` 继续保留。DMA 环保持 6 个 10ms 周期，A～C 的启动预填充为 4 周期，D 候选提高为 5 周期；输入双声道 S16 转换到 48kHz，内置输出混为单声道。

本轮使用 `audio silent` 运行 PCM / 转换 / DMA，同时保持 DAC 静音与功放关闭。实际扬声器音质、音高、暂停恢复的听感未通过静音测试判定；未执行测试音。

### 版本 B 的快速开关前端修订

完成版本 A 存档持久化验收后，前端另修复快速 `audio off` / `audio silent` 的请求合并：启动因取消返回 `-ECANCELED` 时保留最新的启用意图；两个游戏帧之间先 off 再 silent 时保留停止待处理标记，在下一帧先同步清理旧 `sound_started` / PCM 状态，再按最新请求重新启动。帧边界也提前检查整体停止请求，避免停机过程中又启动音频。

这些增量构成版本 B，仅修改音频控制时序；B 的双构建检查与快速开关实板检查已通过，具体结果见后文版本 B 记录。

## 已完成的软件验证

版本 A、B、C、D、D2、E、E2、F、G 和 H 的双构建均通过入口、ABI、未解析符号、DDR 布局与堆预算检查。保存各组身份，避免把早期实板记录误关联到后来更新的构建目录：

| 版本 / 产物 | BIN 长度 | 静态布局后的初始堆 |
|---|---:|---:|
| A / Release | 840432 字节 | 48238592 字节 |
| A / Debug | 1479824 字节 | 47599616 字节 |
| B / Release | 840496 字节 | 48238592 字节 |
| B / Debug | 1479984 字节 | 47599616 字节 |
| C / Release | 841808 字节 | 48234496 字节 |
| C / Debug | 1481984 字节 | 47595520 字节 |
| D / Release | 844272 字节 | 48234496 字节 |
| D / Debug | 1485664 字节 | 47591424 字节 |
| D2 / Release | 845264 字节 | 48234496 字节 |
| D2 / Debug | 1486976 字节 | 47591424 字节 |
| E / Release | 845360 字节 | 48234496 字节 |
| E / Debug | 1487168 字节 | 47591424 字节 |
| E2 / Release | 847040 字节 | 48230400 字节 |
| E2 / Debug | 1489520 字节 | 47587328 字节 |
| F / Release | 847648 字节 | 48230400 字节 |
| F / Debug | 1490336 字节 | 47587328 字节 |
| G / Release | 847840 字节 | 48230400 字节 |
| G / Debug | 1490736 字节 | 47587328 字节 |
| H / Release | 847856 字节 | 48230400 字节 |
| H / Debug | 1490768 字节 | 47587328 字节 |

版本 A 含 64KiB SRAM / Flash 类型识别修订，Release SHA256：`37303ed417544929ad753a835fef923a94c42a0560168a1c0a1d1b4784355664`；Debug SHA256：`0f6908935f919a6498b91cf91679e24f1c6bf901cbe592a7c4e74bcde3da4b44`。构建日志为 `build/save-audio-Release-build-20261017.log`、`build/save-audio-Debug-build-20261017.log`。

版本 B 含后续快速音频控制修订，Release SHA256：`ad4c1e8685dbb3b7518544b3ab014f409a5d76783f07903b0efb1ab12773016b`；Debug SHA256：`8ee04bd951c19f79d3abbf2de888150a5ef97e1c59f36764f678b4259b99604e`。构建日志为 `build/save-audio-Release-final-build-20261017.log`、`build/save-audio-Debug-final-build-20261017.log`。

版本 C 仅增加底层 SD SPI 错误路径记录，Release SHA256：`e29cd5d1adc932440dbb7ea735b33a7761a2abae8d01d84c38290efbeed4236c`；Debug SHA256：`b768a021f75bc642b37b5bcfcd374de8ee7ba1f4e6c48159786d22d7ff5494d2`。构建日志为 `build/save-audio-Release-sdtrace-build-20261017.log`、`build/save-audio-Debug-sdtrace-build-20261017.log`。

版本 D 为后续诊断与音频预填充候选，Release SHA256：`ed86647e8815fcd5fc0d163333c23c40bf8b5ffa979b35bfefa4d3394b03b02e`；Debug SHA256：`7b413287270e617049bf7e022cba70c7c96239fb867fc7c15cc949bea445bb62`。构建日志为 `build/save-audio-Release-D-build-20261017.log`、`build/save-audio-Debug-D-build-20261017.log`，产物归档为 `build/firmware-revisions/save-audio-D-*`。

版本 D2 仅补初始化观测，Release SHA256：`3c185476ec4b89409ab7947d1ff7c2a58053e3fca11da03e18fdafad13a93a9a`；Debug SHA256：`8604ad6644d72ffda0ec713155bd65df2468a61fd9a84d1b4187eb308a37c40f`。构建日志为 `build/save-audio-Release-D2-build-20261017.log`、`build/save-audio-Debug-D2-build-20261017.log`，归档为 `build/firmware-revisions/save-audio-D2-*`。

版本 E 为 SSI 完成时序候选，Release SHA256：`365c5ca7ffdc31bd857f16a5d35c8901ec1cc502de930d3c8a6cf17e39db55ad`；Debug SHA256：`833f92f23575193a17be04105c355284b74028a1c8019f1966b896caac9f22f3`。构建日志为 `build/save-audio-Release-E-build-20261017.log`、`build/save-audio-Debug-E-build-20261017.log`，归档为 `build/firmware-revisions/save-audio-E-*`，包含 `ssi-source.json` 派生来源。

版本 E2 新增最小 GPIO 命令对照，Release SHA256：`a3ae38e1643072169f007fc5a897d6e037a1fe824e0f8c144a9b60124cb38227`；Debug SHA256：`eca94bb9ee85276b4101af842339597540b7ead150972e0a59751ff15da153fc`。构建日志为 `build/save-audio-Release-E2-build-20261017.log`、`build/save-audio-Debug-E2-build-20261017.log`，归档为 `build/firmware-revisions/save-audio-E2-*`，也保留 SSI 派生来源。

版本 F 为 SSI deadline 与首播 PCM 预填候选，Release SHA256：`c7720d655f08168a7df614feaff0a388cb27c22234400c28d366ee94334036fd`，主机 BIN CRC32 `bc34af53`；Debug SHA256：`46507248abfd097bde898389d0471bc6f761af552d2cc4a22369be3bcd7a7a3f`。构建日志为 `build/save-audio-Release-F-build-20261017.log`、`build/save-audio-Debug-F-build-20261017.log`；归档 `build/firmware-revisions/save-audio-F-*` 含 BIN、ELF、manifest 与 SSI 派生来源。

版本 G 为延后 FINISH 的 SSI 候选，Release SHA256：`05171417f5a22a338a1f15763eb9ebb16f4052941b6541a8adaa830167d48470`，主机 BIN CRC32 `a4e730f3`；Debug SHA256：`b51622a8cb352d753aa64276a235c82609d639b64fb1cc865f84fbb67c1c71b6`。构建日志为 `build/save-audio-Release-G-build-20261017.log`、`build/save-audio-Debug-G-build-20261017.log`；`build/firmware-revisions/save-audio-G-*` 归档 BIN、ELF、manifest 与 SSI 来源。

版本 H 为最终 END 等待前清发送欠载候选，当前 Release SHA256：`c5884665369f9fe5813bb2d1b78a2ce9f8fdf0d9ef3a7f5acec0931f8542412d`，主机 BIN CRC32 `1e8f4906`；Debug SHA256：`f3a72d32c95d9cd4afd28c1493ef1f9a28218981bd4fe1ffb9dd85f299be98d0`。构建日志为 `build/save-audio-Release-H-build-20261017.log`、`build/save-audio-Debug-H-build-20261017.log`；`build/firmware-revisions/save-audio-H-*` 归档 BIN、ELF、manifest 与 SSI 来源。二十份产物入口均为 `0x80600000`。

完整主机回归入口 `scripts/test-regba-host.ps1` 已通过，日志为 `build/regba-save-audio-host-20261017.log`。保留存储协议 / FAT32、140 组重采样组合、显示像素和按键消抖的原有回归，并增加以下检查：

- 音频直接提取生产 start / stop / queue 代码，覆盖背压、部分与完整周期提交期间的停止交错、初始化及稳定等待期间取消、静音保持和周期发布时间统计。
- 存档使用生产 writer / loader 与构建目录中的本地文件，覆盖五种大小的首次保存、替换与上一版本保留；短写 / 短读、open / flush / close 错误、删除旧备份失败、旋转 / 提交 / 回滚失败、缺档恢复、忽略临时文件、正式文件优先，以及非法长度和容量不足。

主机检查不访问物理 TF、GPIO、DMA、扬声器或 NOR，不替代板上文件任务 / stdio 桥、实际介质读写及音频调度验收。存档故障注入是在已定义的文件操作边界模拟失败，不包含物理写入中断电。

新派生核心另经 WSL 中的官方 MIPS ISA simulator 执行 120 帧深度回归通过：递归翻译深度峰值 8、结束时为 0，ARM / Thumb / SWI / RAM 代码 / 自修改代码的 mailbox 均正确，VBlank IRQ 为 118。模拟器执行耗时 19.558 秒，日志为 `build/regba-save-audio-sim/simulator.log`。这项验证覆盖核心软件及 ISA 行为，不覆盖板上缓存、RTOS 切换、TF 与音频 DMA。

64KiB 类型修订后增加真实派生核心 fixture 回归，通过 `load_backup()` → `load_game_config()` → 尚未读取时手动保存 → SRAM 高半区写入 → `0x5555` 修改标记 → Flash 解锁升级及编程 → 保存大小检查，并逐字节比较完整数据。该轮再执行上述 120 帧诊断，mailbox 均正确、IRQ 为 118、翻译深度峰值 8 / 结束 0；耗时 24.142 秒，日志 `build/regba-save-audio-sim64/simulator.log`。这证明修订后的软件路径，不将 ISA simulator 的文件 fixture 结果当成 TF 实板结果。

## 版本 A 实板验收

本轮开始时板子运行原 Linux。普通复位的监听窗口超时后，经同一串口确认 Linux 状态并执行软件重启，成功中断 U-Boot 后通过 YMODEM 上传。有效加载日志为 `build/ramload-save-audio-20261017-linux-reboot.log`：840432 字节，板端 CRC32 `11a611b9` 与主机一致，SHA256 为上文版本 A / Release 值，从 `0x80600000` 执行。先前等待超时且未上传的日志不计作固件加载或运行失败。

启动日志 `build/regba-save-audio-boot-20261017.log` 确认 `save task=0`，即后台任务创建成功；TF 挂载 FAT32，62333952 扇区 / 30436MiB、每簇 64 扇区；LCD 已开启，剩余堆 40557792 字节、7 个任务、`muted=1`。后续初始化 audio 服务任务后为 8 个任务。

### 三类客体存档往返

权威完整轮次为 `build/regba-save-roundtrip-complete-20261017.log` 与 `.json`，`pass=true`。32KiB SRAM、64KiB Flash、512 字节 EEPROM 分别执行：客体 A 写入 → 无手动命令等待 2 秒自动保存 → TF CRC → 停止并重载 → 客体 B 改写及运行中手动 `save` → TF CRC → 停止并重载 → 客体 A 再改写后立即 `stop` → dirty flush → TF CRC → 再重载核对持久数据。

该完整轮次开始时三类存档的计数均为 2；自动 A、手动 B、停止刷新 A 分别为 3、4、5。每次重载的启动标记与计数都和保存前一致；重新启动诊断 ROM 后没有执行新的写事件，且全数据检查无不匹配。最终结果如下：

| 类型 | TF 路径 | 长度 | 最终内容 | TF CRC32 |
|---|---|---:|---|---|
| SRAM | `/GBA/SAVES/__save_sram.sav` | 32768 字节 | A / count 5 | `d78efcde` |
| Flash | `/GBA/SAVES/__save_flash.sav` | 65536 字节 | A / count 5 | `ca1097f7` |
| EEPROM | `/GBA/SAVES/__save_eeprom.sav` | 512 字节 | A / count 5 | `5d5536c2` |

CRC 通过板上读整份 TF 文件获得，与主机按 `<marker,count>` 重复记录计算的完整预期 CRC 一致，不仅比较快照状态行。完整轮次新增 9 次保存尝试，结束累计 `queued=15 completed=15 errors=0 busy=0 last_result=0`，无待提交修改；新增次数与该启动此前的测试累计分别记录。

所有运行样本中 LCD `underrun=0`，音频 `queue_under=0 FIFO_under=0 errors=0 timeouts=0 muted=1`，运行时 DMA 完成数持续增长；堆为 40518232 字节保持不变。诊断 ROM 的 PCM 为零值，因而本轮证明静音 DMA 与存档操作能够并行，不替代真实游戏非零 PCM 的压力验证。结束时核心与音频停止、队列清空，音频仍静音。

首轮 SRAM 自动脚本曾因 `status` 查询与一次性 `before-run` 日志串口交错而报告失败，未见固件崩溃或读写错误。脚本已先等待启动 / 停止日志边界再查询，SRAM 重测和上面的完整轮次均通过；最终以完整轮次日志为依据，不把解析失败推定为实板功能失败。

### 普通复位后的持久化

用户普通 RESET 后，再次通过 U-Boot 将同一版本 A 加载到 RAM。日志为 `build/ramload-save-persist-20261017.log`，仍是 840432 字节、CRC32 `11a611b9`、SHA256 `37303ed417544929ad753a835fef923a94c42a0560168a1c0a1d1b4784355664`；启动与挂载证据为 `build/regba-save-persist-boot-20261017.log`。

随后以 `--verify-only` 执行只读复核，`build/regba-save-persist-verify-20261017.log` / `.json` 为 `pass=true`。三类 ROM 从 TF 读取的启动标记均为 A / count 5，完整 CRC 仍为上表的 `d78efcde`、`ca1097f7`、`5d5536c2`。本次启动的 `SAVE queued=0 completed=0` 始终没有增长，证明读取的是前一轮保存在 TF 的数据，没有先重新写入再读取。该项为普通复位与重新加载 RAM 后的持久化，不等同写入过程中断电验证。

| 项目 | 当前状态与通过条件 |
|---|---|
| 新固件 RAM 加载 | 通过；840432 字节、CRC32 `11a611b9`，任务启动正常 |
| SRAM / Flash / EEPROM 客体写入 | 三类通过；客体指令写入、全数据读回及 TF 完整内容 CRC 一致 |
| 2 秒无修改自动保存 | 三类通过；没有手动 `save`，后台完成、重载数据一致 |
| 运行中手动保存与停止 dirty flush | 三类通过；B 覆盖及立即停止后的 A 数据均成功写入、重载一致 |
| 切 ROM flush 失败保留旧游戏 | 代码实现；实板错误注入待执行 |
| 同次 RAM 启动内重载 | 三类通过；自动、手动及停止刷新后分别重载确认 |
| 普通复位并重新加载固件后恢复 | 版本 A 三类通过；`--verify-only` 读回 A / count 5 和相同 CRC，当前启动保存计数保持 0 |
| `.bak` 回退与坏档拒绝 | 文件操作故障主机回归通过；实际 TF 验证待执行 |
| 真实游戏内保存与继续游戏 | 待执行；实际进入游戏保存点，再重载恢复进度 |
| 静音音频与存档并行 | 诊断 ROM 短测通过，DMA / 显示持续推进，四项错误均为 0；真实游戏非零 PCM 待验证 |
| A 的联合长测 | 未执行；后续 B 的带屏长测另见下节，不把 A 短测或旧固件无屏结果代作通过 |

自制存档 ROM 由 `scripts/make-save-test-roms.py` 生成，编入固件为 `/__save_sram.gba`、`/__save_flash.gba`、`/__save_eeprom.gba`；覆盖 32KiB SRAM、64KiB Flash 和 512 字节 EEPROM。开机只读不写，A / B 按下边沿分别写入不同标记并增加持久化计数，整份数据是重复的 `<marker,count>` 小端记录，可逐字节与主机计算结果比较。Flash 命令与 EEPROM 串行比特由客体 CPU 发出，避免仅用宿主修改备用 RAM 来替代介质协议验证。

`scripts/test-save-roundtrip.py` 已作为上述实板往返及普通复位后 `--verify-only` 的入口完成运行。64KiB 类型修订的派生核心 fixture 回归通过，证据见前文 simulator 记录。

## 版本 B 实板进展

版本 B 已实际经 U-Boot 加载，日志 `build/ramload-save-audio-final-20261017.log`：840496 字节、板端 CRC32 `5db134fe`、SHA256 `ad4c1e8685dbb3b7518544b3ab014f409a5d76783f07903b0efb1ab12773016b`，长度与 CRC 一致后执行。启动与挂载证据为 `build/regba-save-audio-final-boot-20261017.log`。

### B 的三类存档回归

B 独立执行完整存档往返，权威记录为 `build/regba-save-roundtrip-final-20261017.log` / `.json`，`pass=true`。先读取 A 保存在 TF 上的 A / count 5，再完成自动 A / count 6、运行中手动 B / count 7、立即停止刷新 A / count 8；各步分别检查整份 TF CRC 并重载确认。

| 类型 | 长度 | B 最终内容 | TF CRC32 |
|---|---:|---|---|
| SRAM | 32768 字节 | A / count 8 | `24493d4e` |
| Flash | 65536 字节 | A / count 8 | `c510eed8` |
| EEPROM | 512 字节 | A / count 8 | `522f5174` |

B 本次启动 `queued=9 completed=9 errors=0`，所有样本静音，音频四项错误与 LCD 欠载均为 0，结束堆为 40518232 字节。该结果属于 B 自身的重测；A 的普通复位后只读结果仍按前节独立保留。

### B 的真实游戏音频控制

`build/regba-audio-lifecycle-20261017-second.log` / `.json` 在 `/gba_game/test4.gba` 上取得 `pass=true`，5 轮正常暂停 / 恢复、5 轮单次串口写入背靠背 `audio off` / `audio silent` 均通过；还覆盖首次启动约 100ms 时取消、稳定等待期间先取消再要求 silent、整体 stop / 再 run。恢复前后分别查询两次状态，确认 DMA、客体帧和 LCD 持续增长，停止后 DMA 与环形队列正确关闭。

36 个状态观察点覆盖上述切换，全程 `muted=1`，确实观测到非零 PCM。音频 `queue_under`、`FIFO_under`、`errors`、`timeouts` 及 LCD `underrun` 均为 0；完整周期最大发布间隔约 16.9～18ms，DMA 最大重装耗时 3～6µs。最后运行样本为 `nonzero=28231`、峰值 32080、DMA 完成数 70，停止后非零计数继续到 31473、DMA 完成数 80，错误仍为 0；堆保持 26165880 字节。结束时核心 / 音频停止、队列清空，保持静音。

首份无 `second` 后缀的生命周期日志因 COM52 仍被加载捕获进程占用而无法打开串口，没有操作开发板；该记录属于主机端占用问题，不计作固件测试失败。以上结果以成功完成的 `second` 日志为依据。

### B 的带屏联合长测：TF EIO 中止

本轮尝试 30 分钟 `/gba_game/test4.gba` 非零 PCM / DMA、LCD 及 TF 读写联合压力，日志为 `build/regba-save-audio-final-soak-20261017-30min.log`。首个 300 秒周期 `rwtest` 返回 `result=-5` / EIO，脚本中止并执行清理，未完成 30 分钟，不能记为通过。最终核心停止在 18287 帧、客体运行计时 306173574µs；音频队列欠载、FIFO 欠载、DMA 错误和超时仍均为 0，LCD 欠载为 0。清理后核心 / 音频停止、保持 `muted=1`。

进行中的真实游戏状态已出现 `/GBA/SAVES/test4.sav` 后台保存完成：`queued=10 completed=10 errors=0 busy=0 last_result=0`，比 B 三类诊断的 9 次新增 1 次；长度 32768 字节、快照 CRC32 `a2d2225d`，`dirty=0 in_flight=0`。这说明 `test4.gba` 确实触发并完成了原生存档写入，不仅停留在自制 ROM 验证；尚未据此判定游戏内特定保存点或进度恢复成功，文件读回与实际游戏流程也分别验收。

中止前两个样本的板端时间为 411151982µs / 471186983µs，客体帧从 7494 增至 11080，DMA 从 12541 增至 18544；非零 PCM 由 4941474 增至 7496799。两次堆均为 26165880 字节，核心栈最小剩余 160280 / 245760 字节，音频四项错误和 LCD 欠载均为 0。此时 `max_ready_gap_us=20916`、`max_dma_restart_us=7`，两项欠载时间指标仍为 0。这些途中样本保留其对应范围，不改变本轮联合压力未通过的结论。

### TF 错误后的复查

在核心 / 音频已停止的状态下，`build/regba-tf-error-probe-20261017.log` 继续观测到 `ls /GBA/TEST` 与 `test4.sav` CRC 读取返回 `-5`；随后新建 `/GBA/TEST/SDTEST10.BIN` 的 131072 字节写入 / 同步 / 读回成功，CRC32 为 `29df376d`。

后续 `build/regba-tf-error-recheck-20261017.log` 中目录读取恢复正常，`test4.sav` 为 32768 字节、读回 CRC32 `a2d2225d`，与先前快照一致；SDTEST10 的 CRC 也再次通过。`build/regba-tf-error-files-20261017.log` 又确认报错时创建的 SDTEST09 与历史 SDTEST00 均为 131072 字节、CRC32 `29df376d`。

目前证据是短暂 TF I/O 失败；现有日志不能定位具体失败阶段或协议原因，也未在上述文件复查中发现内容不一致。音频欠载和错误计数没有增长，不能将此次中止解释为音频欠载。下一步增加底层错误定位并按证据修复，再重新执行联合压力；不通过盲目重试掩盖这次错误。

### 《龙珠大冒险》的显示状态与后续停止

用户确认 `/gba_game/test4.gba` 是《龙珠大冒险》，反馈画面“卡住”，并不确定仅有 Start / A / B 能否到达保存菜单。此前联合压力在 EIO 后由脚本主动停止核心；后续曾静音恢复运行。准备 C 固件前又明确执行 `stop` 和 `audio off`，使画面保留最后一帧。这些停止均有命令与日志，现有证据不能将画面静止直接判为固件异常死机，也尚未确认游戏内保存菜单或特定进度点恢复。

`build/regba-sdtrace-before-reset-20261017.log` 记录 C 加载前的最后一次 B 运行：主动停止时 7152 帧、运行 119752674µs；`SAVE queued=10 completed=10 errors=0`、`test4.sav` 快照 CRC32 `a2d2225d`，无待提交修改。音频四错误与 LCD 欠载均为 0；停止后 `core running=0 audio running=0 muted=1 queued=0`，堆为 26165880 字节、核心栈最小剩余 160280 / 245760 字节。

### 本地 ROM 保存函数只读核查

只读分析用户提供的 `E:/gba/test4.gba`，实际长度 14357872 字节（`0xDB1570`）、CRC32 `2cf3dede`、SHA256 `950b4e1370984c0cf25c51f1c9afd86f0de182a4c77d27e57309c25293ba23c4`。这些值直接从该本地原文件计算；核心日志的 14385152 字节是 ROM 缓冲区按 32KiB 对齐后的大小。文件虽在偏移 `0xD6CD08` 保留唯一 `EEPROM_V124` 字符串，但实际保存函数已经转换为 SRAM 总线访问：

| 文件偏移 / 客体地址 | 已反汇编确认的行为 |
|---|---|
| `0x114608` / `0x08114608` | 读函数将块号乘 8，构造 `0x0E000000 + block*8 + 7`，用 `LDRB` 倒序读 8 字节到 RAM，然后直接返回 0 |
| `0x1146CC` / `0x081146CC` | 写函数构造相同 SRAM 地址，用 `STRB` 倒序写 8 字节，然后直接返回 0 |

两处函数后方仍残留不可达的 `0x0D000000` 串行 / DMA 逻辑；游戏主代码有直接调用上述读取函数和写入包装函数的 `BL`。因此，这份 ROM 的保存访问符合 EEPROM→SRAM 补丁行为，32KiB `.sav` 与其当前实际访问方式相符。仅凭库字符串强制成 EEPROM 会使实际 `0x0E` 保存访问走错路径；本轮保持现有类型，未修改 ROM 或生产代码。

原版与派生核心的 `case 0x0D` 写入始终调用 `write_eeprom()`，其起始状态会把类型切为 EEPROM；读 `0x0D` 也按地址 / ROM 容量选择 EEPROM，不以继承的 SRAM 类型作为阻断条件。加载 `.sav` 识别 SRAM 不会锁死真正的 EEPROM 总线访问。当前本地生成配置也没有匹配该文件修改后标题 `fciq_tgbus` 的强制 EEPROM 条目。后续 C 在停止状态读整份 TF ROM，14357872 字节、CRC32 `2cf3dede`，首次确认与上述 PC 文件整体一致；具体日志见下节。不用本地签名扫描代替实际总线实现和板上文件验证。

## 版本 C：TF 错误定位

C 已完成双构建与产物检查，增加 `SD_IO` 失败记录，用于定位操作、阶段、扇区、命令 / 响应、数据 CRC 及 HAL / 控制器错误状态；仅在错误路径输出。TF 工作频率、重试策略及协议处理未作修复，不能把增加诊断记为 TF 问题已解决。

C 已实际从 U-Boot 加载到 RAM，日志 `build/ramload-save-audio-sdtrace-20261017.log`：841808 字节、板端 CRC32 `f7146db3`、SHA256 与 C / Release 一致，任务启动正常。BIN / ELF / manifest 已归档到 `build/firmware-revisions/save-audio-C-*`。`build/regba-sdtrace-boot-20261017.log` 确认静音设置、FAT32 挂载、TEST00～10 目录读取和 LCD 开启正常；`test4.sav` 读回 32768 字节、CRC32 `a2d2225d`，与 B 一致。新建 SDTEST11 的 131072 字节写入 / 同步 / 读回通过，CRC32 `29df376d`。

C 的联合复现日志为 `build/regba-sdtrace-reproduce-20261017.log`。60 秒样本音频四错误为 0，120 / 180 秒样本已出现 `queue_under=1 FIFO_under=1`，DMA 错误 / 超时仍为 0；对应 `max_ready_gap_us=36243`、`underrun_ready_gap_us=33017`、`max_dma_restart_us=8`、`first_queue_underrun_us=356078878`。同轮用户按键累计为 Start 2、A 37、B 16，且用户确认画面与按键已恢复响应；仅据这些累计样本不能确定按键与欠载的因果关系。该轮不记为音频零欠载通过。

C 的复现轮次已结束，记录为 `build/regba-sdtrace-reproduce-20261017.log` / `.json`。游戏运行后的 300 秒新建 SDTEST12、600 秒新建 SDTEST13，均为 131072 字节、CRC32 `29df376d`，写入 / 同步 / 读回通过，该轮 `rwtest_failed=false`，没有捕获 SD_IO 错误。包含约 41 秒 ROM 加载在内，主机记录时长约 647.98 秒。音频最初 120 秒样本出现的队列 / FIFO 各一次欠载后保持不变，因此该轮不能记为联合无错误通过，也没有替代 30 分钟验收。

### C 的整 ROM 读取错误

随后在游戏与静音 DMA 同时运行时读取整份 `/gba_game/test4.gba`，`build/regba-sdtrace-rom-crc-20261017.log` 捕获 EIO：`SD_IO op=read stage=read-token cmd=17 r1=00 token=64 sector=137714 count=8 done=2`，`io_error=0 hal=0 ssisr=00000098`。这里的 `token=64` 为十六进制 `0x64`，不是正常数据 token。错误后游戏状态仍为 `running=1`；`test4.sav` CRC `a2d2225d`、SDTEST13 CRC `29df376d` 仍通过，见 `build/regba-sdtrace-after-rom-error-20261017.log`。

停止核心并执行 `audio off` 后，同一整份 ROM 读取通过：14357872 字节、CRC32 `2cf3dede`，与 PC 原文件一致，日志 `build/regba-sdtrace-rom-crc-idle-20261017.log`。这是首次直接取得 TF 与 PC ROM 整文件一致的证据。

再恢复游戏但保持音频关闭，仍以相同 3.75MHz TF 频率读取 ROM，又返回 EIO：`read-token cmd=17 r1=00 token=43 sector=134698 count=8 done=2`，`hal=0 ssisr=00000098`，见 `build/regba-sdtrace-rom-crc-game-noaudio-20261017.log`。`token=43` 即 `0x43`。因此读 token 异常也能在无音频 DMA 时发生，不能归因于音频 DMA；现有数据也不足以判定物理链路或软件根因。

## 版本 D：诊断与预填充候选

D 保持 DMA 环容量 6 个 10ms 周期，启动 / 欠载恢复预填充由 4 改为 5 个完整周期。每次完整周期发布时记录 ready 周期数、活动 DMA 状态及软件观察的 DMA age，作为 `last_ready_*` 快照；首次队列欠载触发时，把此前最后一次发布留下的快照锁存到 `first_under_*`。因此，`first_under_*` 描述的是首次欠载之前最近一次发布的状态，不能理解为欠载发生瞬间仍有这些 ready 周期。`underrun_ready_gap_us` 则在每次队列欠载时更新，欠载计数大于 1 时，它与首次快照不属于同一事件。核心另记录音频调用最大间隔和该时刻的 native / pending PCM 数量，用于区分供给间隔与队列状态。

文件层新增 `FS_TEST` / `FS_CRC` 失败阶段，包括打开、写入、同步、关闭、重新读取及 CRC 不一致。SD 层新增 `SD_BAD_TOKEN` 现场寄存器、响应 / token 轮询耗时和同一失败事务后续 16 字节观察；保留原 EIO 返回，不加入重试，不降低 3.75MHz，不修改 SDK。以上是诊断与预填充候选，不表示 TF 根因已经修复。

D 的完整主机回归 `build/regba-save-audio-D-host-20261017.log` 通过；音频专项 `build/audio-D-host-20261017.log` 直接验证生产 service 的 5 周期起播 / 恢复、36ms 生产暂停及首次欠载快照 / 重置通过。双构建及产物检查通过，实板结果尚未取得。

准备 D 前已对 C 正常执行 `stop` / `audio off`，日志 `build/regba-D-before-reset-20261017.log`：核心在 6158 帧、103098597µs 后停止，音频保持静音、队列清空，队列 / FIFO 欠载仍各 1，DMA 错误 / 超时及 LCD 欠载为 0。

D 已实际上传并启动，`build/ramload-save-audio-D-20261017.log` 确认长度 844272 字节、板端 CRC32 `425aeb1c`、SHA256 与上文 D / Release 一致，任务启动正常。但 `build/regba-D-boot-20261017.log` 中首次 TF 挂载返回 `FatFs=3 result=-19`，随后存档 CRC 读取也返回 `-19`；LCD 开启成功。单独一次显式重新挂载仍是相同结果，见 `build/regba-D-mount-recheck-20261017.log`。

重新挂载后的状态为核心停止、音频停止 / 静音，尚未加载任何 ROM；堆 40553696 字节、7 个任务、LCD `active=1 underrun=0`，新增音频与核心时间字段均为 0。本轮只能确认 TF 尚未就绪，现有日志不足以确定初始化在哪一步失败。用户确认复位前后未改动 TF、供电及杜邦线，接线保持原样；不据此推断硬件损坏或接错线。D 的音频连续性、TF 数据读写与重新联合长测均未完成实板验收。

## 版本 D2：初始化失败观测

D2 已完成双构建、产物检查和归档。仅将 `sdspi_initialize()` 各个失败出口接入已有 trace，涵盖 clock / SPI 初始化、命令、R7 / OCR、CSD CRC / 解码以及切换速度阶段。`SD_INIT` 记录实际已采集的有效值，尚未采集的字段标为 unknown；正常路径不增加 SPI 时钟或日志输出。协议、重试及频率均未修改，新增诊断不能视为初始化问题已修复。

D2 已实际经 U-Boot 加载，`build/ramload-save-audio-D2-20261017.log` 确认 845264 字节、板端 CRC32 `3de6405c`、SHA256 与上文 D2 / Release 一致。`build/regba-D2-init-20261017.log` 中本次初次挂载直接成功，没有触发初始化失败诊断；随后存档读取为 32768 字节、CRC32 `a2d2225d`，LCD 开启通过。一次挂载成功不表示初始化问题已修复，D2 没有更改协议。

### D2 的未运行状态下读取错误

在尚未运行游戏的 ROM 加载阶段，`build/regba-D2-real-load-20261017.log` 捕获 `SD_IO op=read stage=read-crc sector=156272 count=2 done=0 cmd=17 r1=00 token=fe crc_card=ffff crc_calc=7c93 result=-138`，HAL 返回与软件 I/O 错误值均为 0、SSI 状态为 `0x98`。ROM 加载返回 `load result=-1 errno=5`。`build/regba-D2-load-failure-status-20261017.log` 确认 `core running=0 frames=0 audio running=0 muted=1`、堆 40553264 字节、7 个任务，说明该错误发生前没有进入游戏或启动音频 DMA。

随后仍在停止状态下读取整 ROM，`build/regba-D2-rom-crc-stopped-20261017.log` 在打开文件所需的目录读取处失败：扇区 24576、CMD17、`r1=ff result=-116` 超时；文件层明确为 `FS_CRC phase=open fr=1 result=-5 bytes=0`。

再显式挂载以重新初始化，`build/regba-D2-init-after-read-error-20261017.log` 捕获 `SD_IO op=initialize stage=command-r1 cmd=0 r1=ff result=-116`，HAL 为 0、SSI 状态 `0x18`；`SD_INIT` 的 R7 / OCR / CSD 均为 unknown，因为本次流程尚未采集到这些值。FatFs 最终返回 3 / `-19`。

D2 该轮结束时 TF 未挂载、无 ROM 运行，音频保持停止静音；D2 的 5 周期预填充尚未取得硬件运行验收。C 中负载开启 / 停止时的对照保留为当时观察，但 D2 已在游戏运行之前复现，不再将故障限定为游戏负载或音频 DMA。随后对 HAL 清理与等待 END 的源码时序作核对，并形成下节 E 候选；现有记录不足以判定唯一实板根因或卡硬件损坏。

## 版本 E：SSI 完成时序候选

依据本地 PM 第 31.7.2.4 节，SSI 的 `BUSY=0` 可以对应 interval、此时 `END=0`；不能只凭 BUSY 已清除就判定完整交换结束。对 SDK 源码核对发现，轮询 `HAL_SPI_TransmitReceive()` 的 RX / TX 软件循环结束后，正常清理会先禁用接收、清 FIFO，未在此之前等待 END。这是可直接复现的软件时序问题，尚未证明它是此前全部实板错误的唯一根因。

`scripts/prepare-ssi.py` 从 SDK 派生 SSI 源到构建目录 `ssi-sdk`，CMake 编译该副本，原 SDK Git 工作树保持干净。E 仅在 `HAL_SPI_TransmitReceive()` RX / TX 循环结束、正常 `DISREV` / FIFO 清理之前等待 END；复用同一个原 `tickstart` / `Timeout`，超时仍走原错误清理和解锁。工程本地 `bus_idle()` 也在同一 SSISR 采样中要求 END 为 1 且 BUSY 为 0，保留原有 100ms 上限。

TF 频率保持 3.75MHz，不加入重试，不通过屏蔽中断改变调度。音频仍是 6 个 10ms 周期、启动 / 恢复预填充 5 周期。E 的变更范围是 SSI 完成时序，音频此前候选仍需单独硬件验收。

`scripts/test-ssi-end.py` 直接提取原始及派生的生产函数，对同一寄存器 fixture 作对照：原始函数按预期报告 `normal cleanup preceded END`，派生函数通过 interval / immediate END、1 / 64 字节、共享 Timeout、HAL_MAX_DELAY、零超时和错误清理 / 解锁场景；同时检查派生补丁匹配缺失或不唯一时拒绝生成。专项日志为 `build/ssi-E-host-20261017.log`。完整回归 `build/regba-save-audio-E-host-20261017.log` 也通过；该测试使用寄存器模型，不替代真实 SSI / TF 运行。

E 双构建与产物检查通过、产物已归档，随后实际上传启动。`build/ramload-save-audio-E-20261017.log` 确认 845360 字节、板端 CRC32 `bb995465`、SHA256 与上文 E / Release 一致，RAM 加载通过。

`build/regba-E-init-20261017.log` 中首次挂载仍失败：`SD_INIT` 的 R7 / OCR / CSD 均为 unknown，`SD_IO initialize stage=command-r1 cmd=0 r1=ff result=-116`，`hal=0 io_error=0 ssisr=00000098`。D2 此前同类初始化失败的 SSI 状态为 `0x18`，此次 E 为 `0x98`；仅保留原始差异，不据此判定根因。E 未通过 TF 初始化或修复验收，软件对照测试通过不代替此实板结果。

### E 的 TF 单独断电对照

用户已拔出 TF 等待 5 秒再插回，保持开发板供电和杜邦线、不复位，RAM 中仍是同一 E。`build/regba-E-card-reinsert-mount-20261017.log` 中 CMD0 返回 idle R1 `0x01`，随后 CMD8 / R1 为 `0xFF`、`result=-116` 超时，`hal=0 io_error=0 ssisr=00000098`，R7 未取得，挂载失败。

仅单独执行一次显式复查，`build/regba-E-card-reinsert-mount-recheck-20261017.log` 的 CMD0 返回 R1 `0x00`，与此初始化步骤预期的 `0x01` 不同，`result=-19 hal=0 ssisr=00000098`。因此不能把 TF 断电后的现象记作恢复稳定，也不能将 E 记为解决 TF 问题。

### 已发现的 VDD 脱落与重新接牢

用户随后发现卡座 VDD 杜邦线已经掉落，已重新接牢，实际为 VDD → J9 pin14、GND → J9 pin6。视觉核对 HALLEY7 底板原理图（本地资料 `Halley7/01_BASEBOARD/01_SCH/RD_X2600_HALLEY7_BASEBOARD_V1.0.pdf`） 第 6 页 USB&DEBUG：pin14 经 R88 / 0Ω 接 3V3P，C110 / 0.1µF 去耦；pin6 与 pin2 / 4 / 8 / 10 共 GND。当前接法符合原理图；pin12 为 NC，不能把所有偶数脚概括为地或电源。用户无万用表，尚无卡座 VDD-GND 电压实测值，杜邦线长度也未给出。

在 VDD 已接牢、仍保留同一 RAM E 的条件下，只执行一次挂载，`build/regba-E-vdd-restored-mount-20261017.log` 仍为 CMD0 / R1 `0xFF`、`result=-116` 超时，`hal=0 ssisr=00000098`，TF 未挂载。重接供电线已完成，但不能据此记为 TF 初始化或稳定性通过。

前述 TF 异常记录需结合这一已发现的 VDD 连接外部条件审视；尚无法精确对应掉线时间与每轮错误，也不能把全部错误反推为同一原因。E 的源代码与寄存器模型结果仍保留，但其为唯一实板根因、或接牢后已经修复 TF，均未获证实。

用户已在 VDD 接牢前提下再次拔出 TF 等 5 秒后插回、不 RESET、保持板供电与 RAM E。`build/regba-E-vdd-reseat-mount-20261017.log` 的单次挂载仍为 CMD0 / R1 `0xFF`、`result=-116`，`hal=0 io_error=0 ssisr=00000098`，`SD_INIT` 的 R7 / OCR / CSD 均为 unknown。该对照没有恢复挂载；当前仍运行 E，核心 / 音频停止静音，TF 未挂载、ROM 未加载。

## 版本 E2：GPIO 命令实板对照

E2 新增最小 `tf-probe`，仍经 filesystem 请求队列由单一文件任务执行；TF 已挂载或有打开文件时拒绝运行。探测先禁用 SSI，再将 PB0 / CS、PB3 / CLK、PB2 / MOSI 配置为输出，PB1 / MISO 为上拉输入；以 mode 0、5µs 半周期约 100kHz 执行 8 次独立的 CMD0 → CMD8 / R7 命令试验，不读写文件。日志仅在 CS 高时输出。

结束时保持 CS 高、CLK 低、MOSI 高，并把初始化状态清为 0；后续正常 `mount` 恢复 SSI。正常路径保持原 3.75MHz、E 的 END 完成判断及原错误返回，不增加正常访问重试。此对照的 GPIO 约 100kHz 与 SSI 初始化约 375kHz 同时改变了传输实现和频率，因此对照结果不能单独归因为某一个因素。

E2 双构建及产物检查通过，未重复无关主机回归；E 先前的 SSI 专项和完整主机测试按原日志保留，不冒充 GPIO 验证。用户普通 RESET 后，`build/ramload-save-audio-E2-20261017.log` 确认 E2 实际上传启动：847040 字节、板端 CRC32 `576ed3be`、SHA256 与上文 E2 / Release 一致。

本轮先执行 `audio silent` / `audio off`，然后在未挂载状态先运行 `tf-probe`。`build/regba-E2-gpio-probe-20261017.log` 中 8 / 8 次 CMD0 的 R1 字段均为 `0xFF`，CMD8 / R7 为 unknown，`passed=0 result=-5`。探测函数在轮询中未收到任何 MSB=0 的有效 R1 时以 `0xFF` 占位，未保存全部原始轮询字节；该结果说明 8 轮均未收到有效 R1，不能据此判断 MISO 每一位恒高，也不记为 GPIO 或 TF 通过。

随后恢复正常 SSI 路径挂载，`build/regba-E2-ssi-mount-20261017.log` 仍是 CMD0 / R1 `0xFF`、`result=-116`，`hal=0 state=1 error=0 ssisr=00000098`；FatFs 为 3、最终 `result=-19`。状态为核心停止、0 帧、存档提交 / 完成 0 / 0、LCD 未开启、音频停止且 `muted=1`。

该 GPIO 试验绕过 SSI，且没有文件读写、游戏、音频 DMA 或 JIT 运行；此次未收到有效 R1 不能归为 FatFs、音频或 JIT 的运行结果，也不足以单独判定卡损坏。用户已确认 CS → J9 pin7、CLK → pin9、MOSI → pin5、MISO → pin3 四根信号线对应正确、杜邦线约 5cm；实际 VDD / GND 针号也已确认，实测电压仍缺。

保持同一 RAM E2 再执行 `tf-probe`、`mount`、`status`，`build/regba-E2-wiring-confirmed-20261017.log` 中 GPIO 8 轮仍均无有效 R1（`0xFF` 占位）；SSI 再次挂载为 `SD_IO failure=2 cmd=0 r1=ff result=-116 hal=0 state=1 error=0 ssisr=00000098`，FatFs 为 3、最终 `-19`。状态仍是核心停止 / 0 帧、存档 0 / 0、LCD 未开、音频停止且静音，COM52 随后关闭。接线确认后的本轮复查也未通过，未取得新的文件读写或音频运行验收。

### USB 读卡器只读对照与资源备份

TF 已放入 USB 读卡器。本次识别为 G: / disk2、USB、Online、MBR，磁盘容量 31914983424 字节；G: 为 FAT32，卷容量 31902400512 字节、空闲 31824347136 字节。同一读卡器的另一槽为 F: / NoMedia / 0 字节，不作为本次卡数据来源。Windows 最初将 G: 报为 Health Warning / FullRepairNeeded，文件枚举成功。

只读备份 `G:/GBA` 与 `G:/gba_game` 到备份文件目录（本地记录 `build/tf-reader-audit/20261017-191623/files`），共 25 个文件、62560624 字节；错误为 0，全部备份 SHA256 与来源一致。逐文件大小、CRC32、SHA256 及源 / 备份比较见 manifest.json（本地记录 `build/tf-reader-audit/20261017-191623/manifest.json`）。这是两个资源目录的文件备份，不是整卡原始镜像。

| 核对内容 | 读卡器读取结果 |
|---|---|
| `gba_game/test4.gba` | 14357872 字节，CRC32 `2cf3dede`、SHA256 `950b4e1370984c0cf25c51f1c9afd86f0de182a4c77d27e57309c25293ba23c4`，与 PC 原文件相同 |
| `GBA/SAVES/test4.sav` | 32768 字节，CRC32 `a2d2225d`，与 B / C 板端记录一致 |
| `GBA/TEST/SDTEST00.BIN`～`SDTEST13.BIN` | 各 131072 字节，CRC32 均为 `29df376d` |
| 三类诊断 `.sav` | SRAM `24493d4e`、Flash `c510eed8`、EEPROM `522f5174`，均与 B 的最终 A / count 8 预期 CRC 一致 |

本地备份中的三类诊断 `.sav` 又与 A / count 8 的完整预期数据逐字节比较：SRAM 32768 字节、Flash 65536 字节、EEPROM 512 字节全部相等，`full_payload_equal=true`、整体 `passed=true`。报告为完整存档数据校验（本地记录 `build/tf-reader-audit/20261017-191623/diagnostic-save-payload-verify.json`）；该项只检查已落盘的本地备份副本，没有再次访问卡或串口。

执行 `chkdsk G:`，没有任何修复参数，完整输出保留为只读检查记录（本地记录 `build/tf-reader-audit/20261017-191623/chkdsk-readonly.txt`）。开头出现 `Access is denied.`，随后文件 / 目录扫描从 0% 完成至 100%，最终报告未发现问题、无需进一步操作，退出码 0。保留初始 Warning 与这次扫描结果两个事实，不据此推断 Warning 来源或完整介质健康；本轮命令未修改卡文件、未修复、未格式化。

### USB 安全弹出并回 SPI 卡座后的复查

用户已从 Windows 安全弹出 G: 并把卡放回原 SPI 卡座，保持开发板供电、原接线、不 RESET。`build/regba-E2-after-usb-reader-20261017.log` 中 GPIO 8 / 8 轮仍未收到有效 R1；SSI 为 `failure=3 cmd=0 r1=ff result=-116 hal=0 state=1 error=0 ssisr=00000098`，FatFs 3、最终 `-19`。外部读卡器可读和存档完整，不代表板端通路已恢复；这次回座复查仍未通过。

该次回座复查结束时 E2 保留在 RAM，核心停止 / 0 帧、存档 0 / 0、LCD 未开、音频停止且静音；TF 未挂载，COM52 命令进程正常退出。后续供电引出点对照包含整板断电，状态按下节继续记录。

### 对照 3.3V 供电引出点：改接并冷启动后仍失败

已查看底板原理图第 5 页（图像核对 `build/j10-power-alternative-audit.png`），确认 J10 pin12 直接接 3V3P、pin4 为 GND。另核对 PCBA BOM 第 66～67 页 Item42 / Qty7 包含 R88，原理图第 6 页的 R88 为常规 0Ω、非 NC，不能把它描述为设计未装。

用户已确认整板断电后，仅把 TF VDD 从 J9 pin14 移至 J10 pin12，再正常上电；GND 仍 J9 pin6、四根 SPI 不变。用户没有备用杜邦线、卡座或万用表，尚无供电电压实测值。

E2 的 847040 字节镜像与 SHA256 再次核对一致，没有新代码或构建。整板断电丢失了之前 RAM E2，先进入原 Linux 正常启动流程。首次 `--reboot-linux` 记录为 `build/ramload-save-audio-E2-j10-power-20261017.log`：5 秒 shell 检查时 Linux 仍输出 Wi-Fi 重试日志，未确认 shell，工具退出 1、提示 `Linux shell not confirmed; no reboot sent`，没有发送重启或上传。这是本次冷启动中的等待，不作为另一个硬件故障。

只读等待 Linux 启动日志为 `build/linux-startup-j10-power-20261017.log`。启动完成后 `uname` 确认成功，软件重启并加载同一 E2，`build/ramload-save-audio-E2-j10-power-ready-20261017.log` 确认 847040 字节、板端 CRC32 `576ed3be`、SHA256 与 E2 一致，RAM 启动通过。

随后 `build/regba-E2-j10-power-probe-20261017.log` 执行 `audio silent` / `audio off` 与 GPIO 对照，8 / 8 轮仍无有效 R1（`0xFF` 占位）；正常 SSI 挂载为 `failure=1 cmd=0 r1=ff result=-116 hal=0 state=1 error=0 ssisr=00000098`，FatFs 3、最终 `-19`。状态为核心停止 / 0 帧、存档 0 / 0、LCD 未开、音频停止且静音；COM52 命令进程退出 0，串口空闲，E2 仍在 RAM。

该次改变供电引出点并整板冷启动没有恢复 TF，不能认定 J9 / R88 为独有根因，也不能据此判定卡损坏。当时记录 VDD 为 J10 pin12、GND 为 J9 pin6、四 SPI 保持原样，板端未挂载；曾准备以卡座正反面和接线实照核查模块 / 针号，照片尚未收到。后续用户主动重接线，结果见下一节，照片不再作为继续文件与音频验证的前提。

### 同一 RAM E2 下重接线后通信恢复

用户反馈重新连接 TF 卡座接线后再试，没有 RESET；`build/regba-E2-rewired-status-20261017.log` 记录板端时间约 493 秒，仍是同一 RAM E2，核心 / 音频停止静音、LCD 未开。没有修改代码、重新构建或重新上传。

`build/regba-E2-rewired-probe-20261017.log` 中 GPIO 8 / 8 轮均为 CMD0 R1 `0x01`、CMD8 R1 `0x01`、R7 `0x000001AA`、`pass=1`，末尾 `passed=8`。随后正常 SSI 路径挂载 FAT32 成功：62333952 扇区、30436MiB、每簇 64 扇区。该项证明当次 GPIO 命令与正常 SSI 初始化 / 挂载已恢复。

用户进一步确认本次具体为 VDD 松动，接牢后恢复了上述初始通信；没有报告新的针号改动。最近确认的供电接法为 J10 pin12、地为 J9 pin6，保持当前线不动。该事实不等于 R88 或某个固定引脚故障，后续负载下仍需独立验证。

### 恢复后的只读数据通过与负载失败

`build/regba-E2-rewired-save-crc-20261017.log` 中四份存档读取均与既有记录匹配：真实 `test4.sav` 32768 字节 / `a2d2225d`、SRAM 32768 字节 / `24493d4e`、Flash 65536 字节 / `c510eed8`、EEPROM 512 字节 / `522f5174`。停止状态的整份 `test4.gba` 14357872 字节 / CRC32 `2cf3dede` 通过，日志为 `build/regba-E2-rewired-rom-crc-idle-20261017.log`。`build/regba-E2-rewired-lcd-20261017.log` 的 LCD 开启也通过。

随后真实游戏静音音频生命周期 `build/regba-E2-rewired-audio-lifecycle-20261017.log` / `.json` 失败：首次约 100ms 时取消通过，首次 silent 恢复出现 `FIFO_under=1 queue_under=0`，`max_ready_gap_us=16994 max_dma_restart_us=3`。计划中的五轮正常与快速切换尚未执行；该轮不能写为生命周期或 5 周期预填充硬件通过。

保持 `audio off`，让游戏与 LCD 运行，再读整份 ROM，`build/regba-E2-rewired-rom-crc-active-noaudio-20261017.log` 在已处理 73728 字节时失败：`CMD17 stage=read-token sector=133524 count=8 done=4 r1=00 token=ff result=-116 io_error=-116 hal=3 state=5 error=0 ssisr=00000098`。这次为 HAL 超时，与此前 HAL 返回 0 时捕获非法 token 的记录不同，不能合并为完全相同的失败现场。

`build/regba-E2-rewired-active-read-error-status-20261017.log` 显示错误后核心仍在运行、LCD 正常；查询时 5369 帧，随后按命令停止在 5492 帧。停止后打开存档又遇到 R1 `0x42` / EIO。`build/regba-E2-rewired-remount-after-timeout-20261017.log` 的显式 `mount` 返回 `-16`，因为 ROM 文件仍打开，没有执行重新初始化；同轮存档 CRC 随后再次读取为 `a2d2225d`。因此不写成重新挂载成功或复位恢复。

该轮结束时 E2 仍在 RAM，核心 / 音频停止且静音、LCD 保持开启。TF 专项形成最小候选：在轮询剩余计数全为 0、交换已完成时，完成判断优先于 deadline 检查，避免已完成却报超时，并补失败现场计数 / 耗时；直接生产函数的主机原始 / 派生对照通过，当时尚待后续固件实板验证。音频另补首播 FIFO 的真实 PCM 预填窗口和模型验证。以上为当时准备中的候选，不作为根因全部确定或问题已解决的证据，后续 F 至 H 构建与实测另记。

## 版本 F：SSI deadline 与首播 PCM 候选

F 的派生 `HAL_SPI_TransmitReceive()` 仅在 TX / RX 剩余计数仍未完成时判断有限超时，避免轮询已完成却因任务切换后的 deadline 检查报超时。保留 Timeout=0、HAL_MAX_DELAY 和 E 原有的 END 等待 / 错误清理；失败 trace 另记录恢复前 `tx_left`、`rx_left`、`chunk`、`hal_us`，避免清理后丢失现场。正常 TF 频率、重试策略与 SDK 原文件不变。

音频首次启动时，CPU 先向 FIFO 放入 16 个真实 PCM 样本，首段 DMA 从同一周期余下的 464 个样本继续，合计仍为 480；后续周期仍 DMA 480 样本，没有重复首批样本。DMA 环仍为 6 个 10ms 周期、起播 / 恢复预填充 5 周期。`status` 新增 `first_fifo_underrun_us`、`first_fifo_completed_periods`，记录首次 FIFO 欠载的时间和当时已完成周期数；不掩盖运行期的 TUR / FIFO 欠载计数。

完整主机回归 `build/regba-save-audio-F-host-20261017.log` 通过，包含生产音频 startup 和 SSI deadline 函数模型对照；音频专项 `build/audio-startup-host-20261017.log` 记录旧版按预期失败、新版通过。双构建与产物检查通过，但模型回归不等同板上 FIFO 或 SPI 已修复。

F 已实际上传并启动，`build/ramload-save-audio-F-20261017.log` 正常退出，确认 847648 字节、板端 CRC32 `bc34af53`、SHA256 与 F / Release 一致，随后执行并正常创建任务。F 的 RAM 加载通过，不再处于等待 RESET 状态。

`build/regba-F-boot-baseline-20261017.log` 中首次 FAT32 挂载成功，62333952 扇区、每簇 64 扇区；真实 `test4.sav` 32768 字节 / CRC32 `a2d2225d`，SDTEST13 为 131072 字节 / `29df376d`，均匹配既有数据。基线状态为 `core=0 frames=0`、存档 0 / 0、LCD 未开、音频停止且静音，全部错误计数与新增 first FIFO 字段均为 0。这是启动后的静止基线，不替代音频运行验收。

F 的停止态整 ROM 已通过，`build/regba-F-rom-crc-idle-20261017.log` 为 14357872 字节 / CRC32 `2cf3dede`；LCD 开启通过，见 `build/regba-F-lcd-20261017.log`。后续实测按下述范围记录，不提前写 SPI 或音频整体解决。

### F 的真实游戏加载失败

`build/regba-F-audio-lifecycle-20261017.log` / `.json` 在加载 `test4.gba` 阶段失败，未进入任何音频启停：`read-token sector=149368 count=2 done=0 cmd=17 r1=00 token=ff hal=3 state=5 error=0 ssisr=00000098 tx_left=0 rx_left=1 chunk=1 hal_us=99379`。核心 / DMA / 输入 / 输出计数均为 0，因此该错误不能写为已运行的真实游戏生命周期结果，也没有取得真实游戏音频通过。

首次尝试内置 SRAM 诊断生命周期也因目录读取 R1 `0x6B` / `0x20` 失败，见 `build/regba-F-audio-lifecycle-embedded-20261017.log`；随后显式挂载的 CMD0 返回 `0x5C`，见 `build/regba-F-remount-after-load-error-20261017.log`。再做 GPIO 对照时，8 轮 CMD0 返回记录为 `77 / 10 / 00 / 03 / 01 / 00 / 00 / 1f`（十六进制，第 5 轮 CMD8 为 `0x7B`），没有通过；紧接的正常 SSI 挂载却成功，见 `build/regba-F-gpio-after-ssi-timeout-20261017.log`。这里只记录中断后协议状态尚未确定，不据此宣称具体 SD abort 语义或接线再次掉落。

### F 的零 PCM 独立生命周期通过

成功挂载后，使用内置 `/__save_sram.gba` 完成静音生命周期，`build/regba-F-audio-lifecycle-embedded-reinit-20261017.log` / `.json` 为通过。36 个观察点覆盖首次取消、5 轮正常停止 / 恢复、5 轮快速切换、稳定等待期间最新请求及 stop/run；全程 `muted=1`，队列 / FIFO 欠载、DMA 错误 / 超时和 LCD 欠载全部为 0，首次队列 / FIFO 欠载时间与 FIFO 已完成周期字段也为 0。

该轮 `max_ready_gap_us=17996`、`max_dma_restart_us=4`、核心最大音频调用间隔 17894µs。终态核心 / 音频停止，DMA 完成 90 次、输入 42337 帧、输出 46080 帧；`nonzero=0 peak=0`，因此严格属于零 PCM 诊断 ROM 的控制 / 转换 / DMA 路径验收。存档提交 / 完成 0 / 0、错误 0，客体写事件为 0，原 A / count 8 和无数据不匹配保持不变，没有新增写卡。

该结果不是实际游戏非零 PCM、33ms 供给间隔复现、声音听感或联合长测通过。F 结束时核心 / 音频停止静音、LCD 开启，下一版 SSI 候选单独记录。

## 版本 G：TX / RX 全完成后 FINISH 候选

G 仅将派生 SSI 函数的 FINISH 从最后一次 TX 后移到 TX / RX 计数均为 0 之后、END 等待之前；超时记录增加 HAL 清理前 `pre_cr0`、`pre_cr1`、`pre_ssisr` 与剩余计数快照。保留 F 音频实现，不改变数据寄存器访问、传输实现、频率、重试或中断设置。

目标主机入口 `scripts/test-ssi-end.py --output build/regba-host/ssi-deferred-finish` 的三组原始预期失败 / 派生通过对照完成，双构建和产物检查通过。该顺序调整是待验证候选，PM 和寄存器模型未证明原 SDK 在真实硬件上必定丢失 RX，也不能据此将此前全部异常定为同一个根因。

G 已实际加载，`build/ramload-save-audio-G-20261017.log` 确认 847840 字节、板端 CRC32 `a4e730f3`、SHA256 与上文 G / Release 一致，RAM 启动通过，不再处于等待 RESET 状态。

`build/regba-G-boot-baseline-20261017.log` 的首轮挂载失败；当时核心 / 音频 / LCD 均未运行。现场为 `SD_IO op=read stage=command-deselect sector=0 count=1 cmd=17 arg=0 hal=3 state=5 error=0 tx_left=0 rx_left=0 chunk=1 hal_us=99872`。HAL 清理后的 SSI 状态为 `0x18`，清理前快照为 `pre_cr0=0x8180 pre_cr1=0x30000030 pre_ssisr=0x5A`。

清理前 TX / RX FIFO 均为空、BUSY=1、END=0、TUNDR=1；接收仍开启（DISREV=0）、RFINE=0、UNFIN=0，8bit / mode 0。该现场处于收发计数已完成后的 END 等待，并有发送欠载状态，区别于 F 的 `rx_left=1`。FatFs 挂载为 1、最终 `result=-5`；后续存档 CRC 返回 `-19`，没有读到存档，不记数据损坏或读回成功。

同份基线日志最后 LCD 开启通过。该次基线结束时 G 在 RAM，核心 / 音频停止静音、LCD 开启、TF 未挂载、COM52 空闲；之后启动 H 的 RAM 监听。文件 / ROM、真实游戏非零 PCM 与联合稳定性均未通过。

## 版本 H：最终 END 等待前清 TX_ERROR 候选

H 仅在 FINISH 后、END 等待前调用 `__HAL_SPI_CLEAR_TX_ERROR`，保留 ROVER 检查，不改变数据数量、频率、DR 访问、传输实现、中断或重试，也不补发 dummy。PM 关于 UNFIN=1 时空 FIFO 进入发送欠载等待、EACLRUN 仅在新 TX 非空时自动清除、TUNDR 写 0 清除的规则，与 G 的 `0x5A` 实测共同支持这一小范围候选。手册没有逐字保证这次具体状态迁移；下述 H 实测按实际通过范围记录，不据此把此前包含 VDD 松动的全部异常认定为单一软件根因。

针对实际 G 生成函数与 `0x5A` 状态，旧 G 在模型中按预期失败，H 通过；生产函数同时覆盖 1 / 64 字节精确收发、END / cleanup 顺序、未完成计数的 deadline、清理前快照，以及 ROVER 仍返回 HAL_ERROR。原始 stdout 为 `build/regba-host/ssi-final-underrun-test.log`。没有重复无关完整回归，F 的完整主机回归按原日志保留；本次目标模型不替代实板 SPI 验收。

H 双构建、产物检查与归档在 2026-10-17 完成。当晚 `build/ramload-save-audio-H-20261017.log` 的普通 RESET 监听于 21:44 超时退出，没有上传，不计为固件加载或运行失败。

### 2026-10-18：H 实际加载与首轮基线

用户重启后，确认 COM52 没有其他进程占用，重新核对归档 H 的 SHA256 与上述身份一致；通过 `--reboot-linux` 自动确认 `uname`、软件重启并上传。`build/ramload-save-audio-H-resume-20261018.log` 记录 847856 字节、板端 CRC32 `1e8f4906`、SHA256 `c5884665369f9fe5813bb2d1b78a2ce9f8fdf0d9ef3a7f5acec0931f8542412d`，校验后启动通过。继续使用原 H，无新代码或构建。

`build/regba-H-boot-baseline-20261018.log` 首次挂载 FAT32 成功，62333952 扇区、每簇 64 扇区；基线状态为核心 / 音频停止且 `muted=1`、存档提交 / 完成 0 / 0、LCD 未开、各错误计数均为 0。`build/regba-H-save-lcd-20261018.log` 进一步确认原 `test4.sav` 32768 字节 / CRC32 `a2d2225d` 一致，LCD 开启通过。

### H 的真实游戏静音生命周期与运行态整 ROM 读取

`build/regba-H-audio-lifecycle-20261018.log` / `.json` 已通过：真实 `test4.gba`、36 个观察点，覆盖首次取消、5 普通 / 5 快切、稳定等待最新请求和 stop/run；观测到非零 PCM，所有样本 `muted=1`，音频队列 / FIFO 欠载、DMA 错误 / 超时及 LCD 欠载均为 0。该生命周期的最大完整周期发布间隔 22071µs、最大 DMA 重装耗时 6µs、核心最大音频调用间隔 20952µs。此项为真实非零 PCM 的静音控制路径通过，听感未验收。

`build/regba-H-active-start-20261018.log` 再运行游戏、LCD 与静音 DMA；`build/regba-H-rom-crc-active-silent-20261018.log` 在上述状态下完整读取 14357872 字节 `test4.gba`，CRC32 `2cf3dede`，与 PC / 备份一致。读取后 `build/regba-H-after-active-crc-20261018.log` 为 6919 帧、DMA 11576、非零 PCM 4482763，音频四错误与 LCD 欠载均为 0。旧版本遇错的运行态完整读取在本轮通过，但单次短测不等于长期稳定性完成。

### H 的真实原生保存与 TF 读回

运行中执行 `save`，`build/regba-H-native-save-readback-20261018.log` 取得 `SAVE complete result=0 bytes=32768 crc32=a2d2225d`，提交 / 完成 1 / 1、错误 0、busy=0，`dirty=0 in_flight=0 requested=0`。随后 TF 读取同文件为 32768 字节、CRC32 `a2d2225d`，完整回读与快照一致。该项证明真实游戏的手动原生保存与读回，不代表已产生并恢复新的游戏内 checkpoint。

`build/regba-H-pre-soak-stopped-20261018.log` 按命令正常停止核心 / 音频，停止在 13359 帧、223659941µs，保持静音，四项音频错误与 LCD 欠载为 0，存档仍为 1 / 1、空闲、无错误。

### H 的 30 分钟联合压力：通过

以 `scripts/soak-regba.py --duration 1800 --rom /gba_game/test4.gba --skip-load --require-display` 执行，复用当前客体状态，不再注入 Start；非零 PCM 热身检查后正式计时。`build/regba-H-save-audio-soak-20261018-30min.log` 以 `SOAK 30min PASS` 结束，进程退出 0；离线汇总为 `build/regba-H-soak-summary-20261018.json`。用户确认本轮长测期间游戏画面正常、无明显异常。

| 正式长测指标 | 实测结果 |
|---|---:|
| 主机 / 板端计时 | 1800.188 秒 / 1800.154975 秒，相差 33.025ms |
| 正式运行状态样本 | 31 份；完整日志另有准备和停止态，共 35 份 |
| 游戏帧数 | 134 → 107653，增加 107519，约 59.72764fps |
| LCD / DMA 速率 | 约 30.27Hz / 100.00028 周期每秒 |
| 音频队列 / FIFO 欠载、DMA 错误 / 超时 | 全部为 0；首次欠载字段也均为 0 |
| LCD 欠载 | 0 |
| 并行 TF 写读 / 旧文件复核 | 5 个新 128KiB 文件和 2 次持久文件 CRC 均通过 |
| 运行样本空闲堆 / 核心栈最小剩余 | 固定 26157688 / 160280 字节 |
| 最大完整周期发布间隔 / DMA 重装耗时 | 18813µs / 7µs |

正式运行期间非零 PCM 和 DMA 持续增长，所有样本保持 `muted=1`；日志没有 SD_IO / 文件错误、PANIC 或负结果。停止前最后运行样本为 107653 帧、DMA 完成 180232 次、非零 PCM 75479641；脚本正常停止在 107660 帧，音频停止、队列清零，LCD 保留最后画面。

除压力脚本自身的音频 / LCD / TF 校验外，独立只读复核同一原始日志的 35 份完整状态：`SAVE queued=1 completed=1 errors=0 busy=0 last_result=0`，路径均为 `/GBA/SAVES/test4.sav`、32768 字节、CRC32 `a2d2225d`；`backup dirty=0 in_flight=0 requested=0`，没有保存失败摘要。这里的 1 次提交来自长测前手动保存，正式长测没有新增原生提交，因此本轮证明保存状态稳定、结束无待提交修改，不能称为持续原生存档写入压力。实际写入与 TF 读回由前述 H 手动保存用例及 A / B 三类型往返独立验证。完整原始日志 SHA256 为 `222f5a50974c93ee8dbe3125a31ff284596fde4d620d59d9e1e502c5ff41279f`。

### H 结束后的目录、CRC 与静止状态

`build/regba-H-final-save-audio-status-20261018.log` 在压力进程释放串口后再次查询两份状态，核心 / 音频均停止，107660 帧、`audio_requested=0 muted=1 queued=0`，音频四错误与 LCD 欠载仍为 0；空闲堆和核心栈余量不变。保存仍为 1 / 1、错误 0、busy=0、最近结果 0，三个待保存字段均为 0。

`/GBA/SAVES` 中正式 `test4.sav` 和 `.sav.bak` 均为 32768 字节，三类诊断 `.sav` / `.bak` 仍在，没有 `.tmp`。实际再次读取 `test4.sav` 为 32768 字节 / `a2d2225d`，本轮最后创建的 `SDTEST18.BIN` 为 131072 字节 / `29df376d`，均与预期一致。COM52 已释放，H 保留 RAM，LCD 保留最后画面，全程没有播放声音或写 NOR。

当前已通过的范围包括三类诊断原生存档往返及普通复位后恢复、真实游戏手动保存 / TF 回读、真实非零 PCM 的静音生命周期、运行态完整 ROM CRC，以及本游戏带屏 / 静音 DMA / TF 的 30 分钟联合压力。商业游戏实际进入保存点后重载恢复进度、可闻音质和暂停恢复听感仍未专项确认；DDR 全容量 / 边界、连续冷启动和更多游戏覆盖仍保留各原阶段的验收边界。各版本软件、实板短测与长测分开记录，不修改原阶段 0～4 的历史结果。

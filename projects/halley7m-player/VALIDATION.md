# 播放器实施验收记录

## 2026-10-19：构建、双核诊断与静音硬解

| 项目 | 结果及证据范围 |
| --- | --- |
| 旧工程基线 | ReGBA Release 构建及完整主机回归通过；未更新 NOR |
| 工具依赖 | 官方 Victory0 r1.1.1 Linux / GCC11.1.0 经 WSL 输出 RV32IMC/ILP32 ELF；MIPS 使用既有 R5.1.6 / GCC7.2；FFmpeg/ffprobe9.0.2 |
| Debug/Release | 两核 ELF/BIN/MAP、嵌入资源及 manifest 通过；MIPS heap_end=0x83c00000，小核入口0x03c00080，独立地址窗口无重叠 |
| 产物拒绝测试 | Debug/Release 真 ELF/BIN 及15个损坏/越界/错误ABI副本，生产核验器共17项通过；未修改原产物 |
| 主机媒体 | 13项通过：真实转码、SAR16:15、视频延后2秒、AVI/AVIX、2GiB以上偏移、截断/越界/AU边界等；另WAV转换通过 |
| 输入/帧池 | 使用生产C和虚拟时钟测试三键、连发、释放互斥、队列、session、768 stride及四槽持有 |
| AMP主机 | 10,000消息、回绕/CRC、满空队列、会话重置；两owner控制状态机的ACK/PING/STOP丢确认/坏序号/超时退出 |
| ILP32 helper | `__clzsi2` 最高位与随机向量；`__udivdi3` Debug/Release 各203,972向量；目标无外部helper/递归，栈16字节且无RVE |
| 只读存储 | 真MIPS编译与主机FatFs接口边界通过；无写卡符号、无HAL_DMA引用。后续真实TF首8MiB吞吐、30分钟会话和CLMT修复后的大AVIX分别验证，按固件版本记录于下 |
| 主核RAM加载 | 71,104B / CRC0eb994d6经YMODEM上传，FreeRTOS串口和heap命令通过。该旧诊断包未含小核 |
| 双核RAM加载 | 真小核嵌入合包；由MIPS接管后复制高端DDR，READY/ACK成功，未在U-Boot期间上传高端区 |
| 频率校准 | 首测575,996,390Hz，原600MHz配置正确FAULT/reset；改576MHz后测575,996,270Hz，通过2%误差门限，tick配置1000Hz |
| 双核生命周期 | 实板3个session，STOP确认后重启；0x12345678和0xa5a55a5a回显一致；音频服务始终关闭 |
| 双核共享PCM区 | 全256KiB在MIPS写入、小核计算CRC并回报，连续两次686dce08一致；运行中重复start正确返回BUSY，无队列重置 |
| 硬解单帧 | 12帧受控AVI的首帧Y CRC684198e1/UV656f30ca，与PC可见NV12一致；解码5632us、DPU确认、停止后heap恢复 |
| 跨GOP管线 | 90帧/3秒受控AVI，144,000 PCM样本；全部解析解码、DPU确认90个描述符；解码总462,262us，DPU欠载0、描述符mismatch0。此记录不证明物理屏幕已出图 |
| 跨GOP首帧 | 90帧片源因编码上下文不同，参考值为Y956c8522/UV90578ad3；板端逐行CRC完全一致，未将stride768的padding计入 |
| 全帧参考链 | 独立`video verify`读取90帧，全部Y/UV CRC与PC逐帧完全一致（含P参考链和三次IDR），无缺失/重复/额外帧；该模式不测呈现帧率 |
| 共享区实板 | `amp shared`连续两次验证262,144字节，CRC686dce08与主核一致，运行中重复启动被拒绝 |
| 内存采样 | 逐帧验证期间最低采样free heap为45,638,024B，结束恢复55,994,696B；这是调用边界采样，未宣称分配器瞬时峰值 |
| 停止回收 | 90帧诊断结束result=0/stop=0，heap=55,994,696B；IRQs栈剩余16,224B |
| 软复位换包 | WDT0复位、同串口打断U-Boot、YMODEM/长度/CRC后运行新包已实板通过；无NOR写入 |
| 物理屏幕 | 初次用户观察黑屏，DSI内建VPG彩条正常，寄存器确认背光/DSI有效；修正四层重复Z顺序为3/2/1/0后，用户确认视频089末帧完整、方向及颜色正常 |
| 启动和稳态时序 | 将首帧及背光104,455us准备归入LOADING，再启动媒体时钟；90帧均确认，稳态30.272fps、late=0、max_late=27,380us、显示欠载0/mismatch0。含背光准备的总IRQ区间28.970fps也保留在日志，没有以稳态值替换它 |

90帧静音片 `probe.avi` 为324,688B，SHA256 `932dd2a989b4a130ed213ee7538d752915e5b8e7e72c08333db9d62e90d29d1a`。已测合包560,544B / CRCc1ac2414，SHA256 `ec010c3673d2a26e1ed1764d7b170e8d01fcecb9a86a59ac9e747e4c2d70127f`。后续源码更新会产生新散列，以每次 manifest 和串口上传校验为准。

原始证据保留本地：`build/ramload-20261019-150702-7bd454.log`、`build/player-mips-only-board-check.log`、`build/ramload-20261019-152048-170e10.log`、`build/player-first-frame-probe.log`、`build/ramload-20261019-153038-c0e2d9.log`、`build/player-amp-video-90-board.log`；不提交完整串口/媒体产物。

## 尚未通过的交付条件

- 手机实拍的1/15/29分钟声光误差及采集偏差测量按用户要求稍后补；尚不声明≤100ms通过。

旧`video`串口诊断以主核时间调度；完整session使用音频样本时钟。当前已通过下述AMP 30分钟板内稳定性，实际声光偏差及其累积仍需手机录像独立验收。

## 2026-10-19：双核音频服务与实际低音量试听

- 协议/共享内存升级为v2，增加独立音频命令和事件环、媒体会话与命令序号。PCM位置单位为48kHz单声道样本；256KiB共享环容纳131,072样本。STOP确认后才清空共享队列；异常时静音、复位小核并保留资源，需整板复位恢复。
- 真实小核消费PCM并独占AIC、Codec、主PDMA。首次10%两秒500Hz短音完成96,000样本，用户听到但认为偏大；5%两次均完整完成但用户认为偏小；随后7%完成96,000样本，用户明确确认合适并设为默认。后续自动测试保持7%。百分比是Codec档位映射，并非线性声压。
- 在7%下，暂停测试于24,480样本冻结100ms，前后计数相同；恢复后96,000样本全部完成。另分别完成1、15、16、17、479、480、481、5,003样本的短尾测试，补零不计入媒体样本数。每次STOP=0、DMA错误/超时、队列欠载、FIFO欠载均为0，结束heap均为56,289,608B。
- 这些结果验证了共享PCM传输、实际发声、周期尾段和暂停冻结；不替代真实TF/WAV、长期稳定性或音画同步验收。原始日志保留本地`build/player-tone-volume7-board.log`和`build/player-audio-volume7-lifecycle-board.log`。
- 小核SDK的阻塞`ulTaskNotifyTake(..., timeout)`会在critical内yield后留下中断屏蔽，首个READY后任务不再前进。工程改用非阻塞通知获取和`vTaskDelay`，未改SDK源；重新RAM加载后READY、ACK、PING和上述音频实测通过。

## 2026-10-19：三页GUI、TF文件和完整短会话

- LVGL8.3.10仅构建核心，保留34869原始Unifont字形，按需放大；headless真实LVGL、显示driver、按键及回调测试通过。用户确认实际中文页面、方向及长按菜单正常。GUI单任务栈16KiB，首次运行最少剩余2970words；固定5ms扫描回调只操作输入队列，不进入LVGL。
- 首次仅音频诊断包挂载TF出现CMD0/R1=ff无响应；整机重新RAM启动后挂载正常，SSI数据时钟18.75MHz，真实根目录读取成功。未根据首次失败盲改引脚或声称已定位物理原因。
- 实际`0:/multimedia/audio/acceptance-tail-96017.wav`完整输出96017样本，包含最后17样本，EOF状态ENDED、underruns0、默认volume7。
- 整机首轮同步片为90decode/63display/27drop，音频全144000且无错误；定位为GUI独立刷新占用DPU周期，并修正连续播放优先合并、待显期间预解码下一帧。33.033ms主机EOD回归达到90/90/0后重测实板：90/90/0、late0、max_late6666us，audio144000、全部音频错误0；29个GUI请求合入视频，video_busy0，EOD33.014..33.053ms，提交至确认最大32.943ms。
- 高运动TF AVI前缀实测：暂停、继续、STOP，36.028s记录窗口（含准备和暂停），有效音频948480样本，decode594/display593、drop0/late0、max_late26666us；取消时保留的下一解码帧不计显示。音频underrun/DMA错误/超时0；最低采样MIPS free31681936B、RV127648B，短窗口平均忙率MIPS30.89%、RV1.32%。此为前缀测试，不能替代10/30分钟记录或AMP收益结论。
- STOP后codec分配/释放计数相同，GUI继续运行。原始日志本地保留于`build/player-session-scheduling-fix-board.log`、`build/player-tf-avi-prefix-board.log`与`build/player-prefix-measurement-trace-board.log`，CSV与时序只在停止后导出。

## 2026-10-19：真实TF只读吞吐与长跑状态

- `tf bench 0:/multimedia/video/acceptance-30min.avi` 已实板完成，文件大小593,023,512B，仅读取文件开头8,388,608B，分512次、每次16KiB。`result=0`、`eof=0`、`truncated=0`；这是前缀读取成功，不是整文件或跨AVIX验收。
- 全部读块及块间一个tick让出共耗时4,609,434us，壁钟吞吐为1,819,878B/s；纯读调用累计4,289,287us，对应1,955,711B/s。单次read最小8,285us、最大8,907us。窗口不包含挂载、打开、关闭和串口结果输出；原始实板证据为本地`build/player-tf-throughput-board.log`，主机边界测试不作为该速率的证据。
- AMP的1,800秒媒体长跑及同片源600秒单核/AMP对照现已完成，板内计数与CSV结果见下。8MiB吞吐测试仍只代表该前缀读取窗口；手机声光同步按用户选择后续补测。

## 2026-10-19：AMP 30分钟板内稳定性

- 片源为真实TF的`0:/multimedia/video/acceptance-30min.avi`，默认volume7。完整会话到达ENDED，音频86,400,000/86,400,000有效样本，解码54,000帧、呈现54,000帧，drop=0、late=0、max_late=26,666us；音频underrun、DMA错误/超时、队列欠载和FIFO欠载全部为0。
- 正常固定GUI验证序列完成15/15轮，`aborted=0`、`cancel_may_emit_short=0`；结束查询为3107次render及3107次present、GUI error0、GUI栈high-water余量2926words。该注入结果验证同序列GUI负载，不代替全部物理按键人工回归。STOP后会话回到IDLE、kind0、pinned0，音频inactive，仍保留完整样本与帧计数。
- 测量在PLAY命令约3秒后启动，因此结束于EOF时共1797条记录、1,797,734ms，目标参数虽为1800秒，实际CSV记录窗口不足1800秒。固件整段摘要忙率为MIPS3980bp（39.80%）、RV175bp（1.75%）；这些值与完整媒体播放计数、独立600秒窗口分别记录，不混用。
- `build/player-board-analysis/amp30-r2-window600.md` 对测量相对时间0..600秒窗口的CSV核验为完整：MIPS按599.235秒有效CPU窗口加权为39.941%，RV按599.472秒为1.753%；窗口有效`heap_free`最低观测分别为31,661,128B、127,648B。两核窗口边界不同步，CPU口径为scheduler idle，ISR归被打断任务；同片源单核对照见下。
- 本轮固件为2,046,432B、CRC32 `58178d21`、SHA256 `b2c7450017fe04f38f9726244c8f22e4e5e5d3036f82771b8a5e2c27142f7e56`。原始本地日志`build/player-amp30-r2.log`的SHA256为`8aa2f3a05a7051d304d80d60f17ec0b6c48392d5dc01172a9a6740812f48e957`，含ENDED、音频、GUI、序列完成、STOP和CSV证据。

本轮通过的是CLMT修复前固件的AMP 30分钟板内稳定性；修复后固件的大AVIX及实体按键结果独立记于下。手机声光同步仍待验，尚未完成全部交付验收；下述MIPS采集结束阶段的窗口外变化另行保留。

## 2026-10-19：同片源600秒单核/AMP对照

两组均使用`acceptance-30min.avi`、7%音量和同一固定GUI序列。MIPS独立采集为600条、600,005ms、`reason=duration`，GUI完成5/5轮、`aborted=0`；AMP取上节30分钟记录中的前600秒，期间同样完成5轮GUI序列。严格选取CSV的`elapsed_ms`在0..600,000范围内，每组599条；MIPS最后一条600,005ms不纳入此窗口。

| 600秒窗口观测 | AudioOwner=mips | AudioOwner=riscv（AMP） |
| --- | ---: | ---: |
| MIPS加权忙率 | 37.074% | 39.941% |
| MIPS有效CPU窗口 | 599.283s | 599.235s |
| RV加权忙率 / 有效窗口 | RV保持reset，无有效窗 | 1.753% / 599.472s |
| 最低观测MIPS空闲堆 | 31,665,224B | 31,661,128B |
| 最低观测RV空闲堆 | 不适用 | 127,648B |
| 冻结窗口drop / late | 0 / 0 | 0 / 0 |
| 音频underrun、DMA错误/超时、队列/FIFO欠载 | 全0 | 全0 |
| 音频进度事件差分（发送 / 接收） | 无IPC，0 / 0 | 5981 / 5981，约10次/s |

CPU使用各核实际有效`window_us`加权，核窗口边界独立；scheduler idle口径将ISR时间记到被打断任务，idle期间ISR不单列busy。表中堆值取窗口有效`heap_free`的最小观测，不宣称分配器瞬时峰值。MIPS忙率在本轮AMP下增加约2.867个百分点，最低观测空闲堆少4096B；没有证据称AMP降低了MIPS负载或提高稳定性。两组窗口音频错误和drop/late均为0，也不据此推导未测场景的稳定性差异。

冻结窗口与结束阶段分别保留：MIPS结束前会话查询drop=0；冻结的600条测量摘要与CSV中drop/late均为0。其后长CLI摘要输出及STOP阶段新增drop1、late1，最大迟到53,333us，STOP后会话IDLE、kind0、pinned0、音频inactive。这个窗口外问题不能删掉，也不混入前600秒CPU/视频对照。采集器现已把长摘要移至IDLE后输出，12项主机测试通过；尚未用该修正采集器重跑实板。

MIPS本轮固件为2,035,536B、CRC32 `2c877f19`、SHA256 `975f1d0ee256d7182d7a261400d3d2deef60c6007eea0e20c053aa4fef374a2d`。原始日志`build/player-mips10-formal.log`的SHA256为`bd3b241c4171a9be53a183c6ddbabd750604dde5569b04a63700fd637a370b76`；AMP原始日志及散列见上一节。以上CPU值和零错误边界已直接按两份原始CSV及真实`mips10`/`amp30`标签核对。手机声光测量按用户选择延期。

## 2026-10-19：大AVIX首轮呈现失败与CLMT修复复测

- 实际TF文件为2,147,591,940B，包含三段RIFF。首轮完整扫描90帧、144000样本；播放到ENDED时解码90帧、呈现82帧、drop8、late8、max_late120,000us。音频144000/144000样本且所有音频错误为0，STOP后heap恢复45,756,248B。证据为本地`build/player-avix-over2gib-board.log`及`build/player-avix-trace-board.log`。首轮只确认跨大偏移的数据可读与解码完整，呈现验收失败。
- trace中两段owner事件包围的空档分别为146.487ms、151.650ms，掉帧为18..21和38..41；这些空档没有单独测量seek函数，不能称为精确seek耗时。IDR解码约5.6ms，定位到read-ahead提前遇到JUNK跳跃时的FatFs簇链遍历。
- 修复在至少1GiB的真实TF文件LOADING阶段、音频启动前建立4096DWORD（16KiB）的CLMT；每256个FAT步骤维护AMP/取消并让出一个tick，容量不足明确返回`-E2BIG`，close释放。小文件和`@embedded`不分配该表，只派生build内FatFs，SDK原件不改。真实FatFs虚拟FAT32、storage边界及session失败/取消测试通过；两核Debug/Release、MIPS owner真实构建与相关每组17项产物核验通过。
- 修复后Release包为2,048,864B、CRC32 `91cbfe5d`、SHA256 `8dbf8f327540ad7646338a921531b190246c047ef677eda4502af46b27e6f95e`；`build/player-avix-fastseek-final-ramload.log`确认RAM上传长度、板端CRC与SHA完整匹配，未更新NOR。`build/player-fastseek-full-host.log`完整主机回归通过，含真实FatFs新增测试；两架构Debug/Release与MIPS owner均已真实构建。
- 新包实板复测日志`build/player-avix-over2gib-fixed-board.log`记录ENDED时90帧解码、90帧呈现、drop0、late0、max_late13,333us；音频total/written/completed均为144000，全部音频错误为0，volume7、pinned0。STOP后IDLE、音频inactive，播放前后heap均为45,752,152B，跨大偏移的本片播放呈现与回收通过。
- 本次CLMT诊断为enabled1、capacity4096、required/used6，LOADING建表335,417us；后续仅观测到3次实际seek，`max_seek_us=2`只代表这3次调用，不推定其他文件、卡或所有seek均为2us以内。
- 独立逐条核对修正版340条RAM trace：90次DECODE、SUBMIT、LATCH分别完整覆盖0..89，DROP/BUSY事件为0。旧两处LATCH16→17、36→37的165.169/132.133ms间隔均变为33.034ms；frame1..89的相邻latch间隔范围33.030..33.037ms，每次仅1个IRQ，旧百毫秒停顿消失。
- 既有30分钟AMP与600秒单核对照都来自CLMT修复前固件，仍对应上文原SHA。旧产物已备份于`build/player-evidence-before-fastseek/amp-release`、`amp-debug`和`mips-release`；新包AVIX结果为本节独立记录，不将旧包30分钟结果记到新SHA。

同一新包另完成普通593,023,512B AVI的45,000ms冻结窗口回归，45条CSV、一次完整菜单/诊断GUI循环，窗口drop/late及音频错误均为0。普通文件CLMT disabled、未分配映射表，4次实际seek最大455us。随后WAV完整输出96,017有效样本，含末17样本，音频错误0，STOP后IDLE、inactive、pinned0。证据为本地`build/player-post-fastseek-regression-board.log`。

该短回归在冻结窗口后集中查询状态并STOP，仍记录到窗口外drop1/late1、max_late33,333us；这说明把长摘要后置并未消除集中UART查询的全部影响。正式窗口内的GUI操作没有持续掉帧，窗口外变化如实保留，不以短回归替代旧版本的30分钟记录。

## 2026-10-19：实体三键人工检查与最终状态

用户按人工步骤检查媒体库中A/B连续短按、按住连发及释放停止，Start进入目录/媒体、播放暂停/继续、长按菜单、诊断浏览/返回及停止返回，明确回复“全部正常”。这轮确认覆盖实体输入、页面及暂停/菜单显示；手机声光同步仍按用户安排后补。

人工操作后的只读查询为会话IDLE、音频inactive、pinned0、GUI error0、输入丢弃0，AMP error/rejected0。当前会话设置曾为10%，已在停止状态恢复为7%，未再启动播放；下一次START采用该值，源码和manifest默认同为7%。inactive音频状态保留上一已停止会话的音量10与96017样本统计，不作为当前播放参数。最后GUI位于诊断页、菜单关闭。证据为本地`build/player-final-manual-status-board.log`与`build/player-final-volume7-confirmed-board.log`。

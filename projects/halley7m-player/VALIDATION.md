# 播放器实施验收记录

## 2026-10-20：NOR安装与复位自动启动通过

用户明确要求将当前RAM播放器写入Flash。源R4合包1920032B/CRC1e71d376保持不变，
追加406496B FF形成2326528B（0x238000）NOR派生包，CRCa046b4d8，SHA256
`2a625f88915fcb01bfdc52656bbdedc9befbea5ffddf59eca65875c22c297a58`。
本地核验真实ELF/BIN、BSS边界、旧ReGBA178853ea及双份16MiB原始备份；
新增包装/故障模型和既有完整NOR写入模型通过，默认dryplan不打开串口。

实板匹配旧完整应用区及持久环境后，擦写`[0x40000,0x278000)`，全部2326528B
逐字节回读一致，FF尾和完整环境CRC/bootcmd通过。原SPL/U-Boot代码CRC3a3c3918、
本次rootfs CRC8618c73f均保持不变。持久命令读取0x238000到0x80600000后go；
软件reset已观察到从NOR装载、PLAYER_BOOT、AMP_READY和GUI_INIT=0。

独立启动/播放冒烟初始为IDLE、媒体库页、音频关闭，默认音量7%。三秒横屏MP4
90帧全部解码/呈现、drop/late0、max_late13333us，144000有效样本完整且音频错误0；
STOP后IDLE、kind0、pinned0、音频CLOSED/inactive，GUI27/27且错误0。
原始日志及散列见[NOR安装说明](NOR_INSTALL.md)。实际断电上电由用户随后拷卡测试，
安装JSON的`cold_boot_verified=false`保持如实；兼容副本test-player.mp4尚未做TF实播。

## 2026-10-20：横屏MP4实时呈现与native回切通过

R4修正CUSTOM/Rotator重复扫描、残留crop/scale、SOF瞬间站点滞后及停止顺序。
三秒RAM横屏MP4为90帧全部解码/呈现，drop/late0、144000有效AAC样本完整，
全部音频错误0。真实TF的两份30秒1280×720 MP4分别带48/44.1kHz双声道AAC，
各900帧全部解码/呈现、drop/late0、1440000有效样本准确，EOF与STOP状态核对通过。
原始正式结果为`build/player-formats-mp4-r4-paced-board.log/.json`；
此前未节流的75B串口长命令丢尾、未开始播放，该次失败日志独立保留，
工具修复已提交`271c881`，没有覆盖失败或改变固件来迁移测试结果。

宽幅→WAV96017样本→native跨2GiB AVIX的回切也已通过，AVIX为90/90帧、drop/late0、
144000样本完整且音频错误0；最终IDLE、kind0、pinned0、音频CLOSED/inactive，
空闲堆45760344B，GUI85绘制/85确认且错误0。两TF MP4首例由native切wide后，
停止媒体时宽幅GUI仍持有三块CUSTOM，空闲堆减少11060760B；第二例同路线差值0，
不能把GUI仍持有的输出池当作媒体泄漏。用户已确认横屏末帧089完整、方向颜色正常。

44.1kHz MP4暂停时两次查询均冻结在192960样本，恢复后900/900帧、1440000样本完整，
drop/late0、音频错误0。独立35秒GUI按键序列在PLAYING启动，暂停20秒后恢复，
完成菜单/诊断往返1/1次、无abort，视频仍900/900、drop/late0；这属于自动注入。
最后再次切native AVIX并STOP，空闲堆恢复45760344B、音频关闭，GUI354/354且错误0，
媒体任务累计最低剩余11322words（45288B）。原始记录分别为
`build/player-formats-rotation-r4-pause-ui-board.log`、`-ui-cycle-board.log`及
`-final-native-board.log`；暂停状态启动UI序列被门槛拒绝的原始结果也保留，未算通过。

本轮候选1920032B、CRC `1e71d376`，SHA256
`2ff6503f4650481d6339c64c21ecbe39e9670f93cd1f1d0977ba9a8abe1553c6`，
RV35880B、协议4/布局3，保存在`build/player-format-evidence/rotation-r4`。
RAM上传有板端长度与CRC校验，未写NOR。双核Debug、AMP Release、MIPS owner Release
均已构建核验；真实Release ELF/BIN及损坏副本17项检查、完整GUI/显示主机回归通过。
此前十二音频用的是R2候选，下节保留其散列，不将结果无条件记到R4。
逐轮失败、实际事件相位、统计及日志SHA见[显示实板摘要](display/BOARD_VALIDATION_20261020.md)。
本轮短片结果不作为新格式30分钟长跑或实际声光偏差数值证明。

## 2026-10-20：MP3、AAC、M4A 十二组合实板通过

真实TF的MP3、ADTS AAC-LC、M4A各覆盖44.1/48kHz、单/双声道，12个独立约30秒
会话均到ENDED/DRAINED。累计17281552有效样本，每例session和audio计数精确一致；
音频underrun、DMA错误/超时、queue/FIFO欠载均0。用户轮播期间回复“能听到，声音正常”。

每例STOP后IDLE、音频CLOSED/inactive、pinned0，空闲堆前后均45764440B，十二例差值均0。
媒体96KiB栈累计最小剩余11322words（45288B）。四例M4A的有界CLMT实际运行并释放。
完整逐例计数、样本裁剪解释、日志摘要与证据范围见
[音频实板摘要](audio-decode/BOARD_VALIDATION_20261020.md)。本轮是12次独立会话，
不称连续六分钟，也不替代新格式30分钟长跑。

该结果对应1917840B、CRC `6635efcb`、SHA256
`70220c27b013bbf9d366b30c52150dc72f24d81617a295486a0672a07b9ebb55`候选，
原ELF/BIN/MAP/manifest已保存`build/player-format-evidence/rotation-r2`；上传日志确认
板端长度与CRC一致。新包的旋转测试仍未通过，不能把音频结果扩展成MP4视频验收：

- 原27aca候选在横屏首帧后因同表面重复SOF触发`-EINVAL`，保留资源，音频未START。
- 本70220候选对held SOF已正常处理：rotator错误0，SOF/EOF持续递增。三秒MP4的
  90帧全部解码，但仅呈现1帧、drop/late各88，GUI-only第二次写回未完成，不能到ENDED。
  AAC144000样本完整且错误0；STOP后音频关闭，显示资源pinned1，未算停止回收通过。
- 原始显示证据是`build/player-formats-rotation-r2-probe-board.log`和
  `build/player-formats-rotation-r2-display-board.log`。源码复核发现HAL会保留并修改
  crop/scale，旧native720宽裁剪污染wide1280的GUI层；对应修复和复验正在进行。

## 2026-10-20：新格式固件 RAM 启动，串口连接待恢复

用户已将14个新格式样片复制到TF的`multimedia/audio`及`multimedia/video`。
COM52最初未收到启动输出，USB串口转接器重插并重新枚举后捕获U-Boot，完成
新包的YMODEM上传、长度与板端CRC校验，未写NOR。原始证据为本地
`build/player-formats-20261020-ramload-r2.log`及同名`-console.log`。

- 上传1916832B，CRC32 `2bd32c36`，主机SHA256
  `27aca027e920a2b01763b323a0e1591b0463c50cd686bb1a0d6bdec1e5ef2334`。
- 应用`PLAYER_BOOT`后加载35880B小核，`AMP_EVENT opcode=256 value=4`及
  `AMP_READY`确认协议v4双核握手；小核计时器实测575993921Hz，配置576MHz。
- `PLAYER_NOTE`默认音量7%、启动音频关闭；`DISPLAY_INIT=0`、`GUI_INIT=0`、
  `BUTTONS_INIT=0`，并观察到SD-SPI协商频率18750000Hz。这些不是新媒体读取或播放证明。
- 启动后准备读取状态时COM52不可打开；连续两次系统枚举中FTDI COM52消失，
  尚未执行任何PLAY。已请用户保持开发板供电，仅重新插稳USB串口转接器；
  保留RAM固件，恢复连接后继续样本计数、发声、横屏旋转和实时性能验收。

新增14例自动板测入口及10组假UART回归已完成，工具提交`ae41828`。
本轮只确认新固件RAM启动及协议v4握手，未将串口中断前的初始化记作格式播放通过。

## 2026-10-19：MP3、AAC/M4A、横屏 MP4 主机集成

用户已选择 MP3、AAC/M4A、MP4（H.264＋AAC），并要求普通 1280×720 横屏
文件板端旋转；手机录像及实际声光偏差数值验证已取消。实现范围见
[FORMATS.md](FORMATS.md)，默认音量保持7%，未自动提高。

- 解码器：16个真实片源的源采样率PCM逐样本/同轴误差对照、9率转换、背压、
  EOF及源码SHA通过，MIPS Debug/Release真编译与栈报告完成。MP3峰误差1LSB；
  AAC fixed最差SNR49.148dB。证据为本地`build/player-audio-decode-test.log`和模块报告。
- MP4：25项通过，包括17个真实布局/码流、短读、7个拒绝用例；规范化视频PC
  逐帧CRC与原文件一致，AAC逐字节一致，时间/偏移与ffprobe对照。模块范围
  不放宽会话对CFR30/Baseline/单参考/无B帧的限制。
- source：24个真实文件及16个错误用例通过，覆盖ID3/Xing/LAME、AAC priming、
  edit时长舍入、三字节短读与七样本输出背压。计数及PCM对照报告为
  `build/player-host/source/comparison.json`。
- 会话：真实MP3、ADTS、M4A、1280×720 MP4分别三次暂停/恢复/EOF/STOP通过。
  1秒MP3/M4A/MP4各48000有效输出样本，ADTS按实际解码及转换为49040；
  MP4每轮30帧全部解码，音频peer错误0。另覆盖布局连续EAGAIN期间STOP和
  换WAV，不超时保留资源；旧WAV/AVI、周期33.033ms显示模型回归通过。
- 显示：生产driver、CUSTOM三槽、EOW/SOF/EOD持有、合并IRQ、末帧重复、
  GUI异步排空/布局ACK、停止超时保留以及实际LVGL22次提交通过，真实MIPS库通过。
  此项为主机模型，仍需核对实际硬件事件、方向和720p30。
- 完整主机入口`build/player-formats-all-host.log`通过，最后会话边界修复后独立
  回归见`build/player-formats-session-host.log`。双核Release/Debug及MIPS owner
  Release均已构建并核验；新Release真实ELF/BIN及损坏副本17项检查通过。
- 14个30秒板测样片、合计5124077B已生成，参数与SHA见本地
  `build/player-fixture/formats/manifest.json`；两个MP4均900帧、未预旋转。

本轮候选AMP Release合包1916832B（含三秒横屏MP4诊断资源），CRC32
`2bd32c36`，SHA256 `27aca027e920a2b01763b323a0e1591b0463c50cd686bb1a0d6bdec1e5ef2334`，
协议4、共享布局3、RV35880B。2026-10-19 COM52发送status超时，尚未进入
上传，未运行这份新固件，也未写NOR。已请用户将新样片复制到TF并保持断电；
新格式真实发声、TF读取、旋转刷新、任务栈余量及稳定性均待板测，不算通过。

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

## 2026-10-19：验收范围调整

用户明确取消手机录像及实际声光偏差数值验证，原计划的1/15/29分钟录像与采集偏差校准不再是交付条件。基本音画同步、连续稳定播放仍是当前目标；未做手机实拍不再构成交付阻塞或未通过项。

实际声光偏差尚未测量，不声明≤100ms已通过。相关分析工具和合成测试保留为可选诊断。旧`video`串口诊断以主核时间调度；完整session使用音频样本时钟。下述AMP 30分钟板内稳定性与人工检查按各自证据范围记录，不由板内计数推定实际声光偏差数值。

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
- AMP的1,800秒媒体长跑及同片源600秒单核/AMP对照现已完成，板内计数与CSV结果见下。8MiB吞吐测试仍只代表该前缀读取窗口。

## 2026-10-19：AMP 30分钟板内稳定性

- 片源为真实TF的`0:/multimedia/video/acceptance-30min.avi`，默认volume7。完整会话到达ENDED，音频86,400,000/86,400,000有效样本，解码54,000帧、呈现54,000帧，drop=0、late=0、max_late=26,666us；音频underrun、DMA错误/超时、队列欠载和FIFO欠载全部为0。
- 正常固定GUI验证序列完成15/15轮，`aborted=0`、`cancel_may_emit_short=0`；结束查询为3107次render及3107次present、GUI error0、GUI栈high-water余量2926words。该注入结果验证同序列GUI负载，不代替全部物理按键人工回归。STOP后会话回到IDLE、kind0、pinned0，音频inactive，仍保留完整样本与帧计数。
- 测量在PLAY命令约3秒后启动，因此结束于EOF时共1797条记录、1,797,734ms，目标参数虽为1800秒，实际CSV记录窗口不足1800秒。固件整段摘要忙率为MIPS3980bp（39.80%）、RV175bp（1.75%）；这些值与完整媒体播放计数、独立600秒窗口分别记录，不混用。
- `build/player-board-analysis/amp30-r2-window600.md` 对测量相对时间0..600秒窗口的CSV核验为完整：MIPS按599.235秒有效CPU窗口加权为39.941%，RV按599.472秒为1.753%；窗口有效`heap_free`最低观测分别为31,661,128B、127,648B。两核窗口边界不同步，CPU口径为scheduler idle，ISR归被打断任务；同片源单核对照见下。
- 本轮固件为2,046,432B、CRC32 `58178d21`、SHA256 `b2c7450017fe04f38f9726244c8f22e4e5e5d3036f82771b8a5e2c27142f7e56`。原始本地日志`build/player-amp30-r2.log`的SHA256为`8aa2f3a05a7051d304d80d60f17ec0b6c48392d5dc01172a9a6740812f48e957`，含ENDED、音频、GUI、序列完成、STOP和CSV证据。

本轮通过的是CLMT修复前固件的AMP 30分钟板内稳定性；修复后固件的大AVIX及实体按键结果独立记于下。手机录像及数值验证已由用户取消必测，不再作为本轮交付阻塞；下述MIPS采集结束阶段的窗口外变化另行保留。

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

MIPS本轮固件为2,035,536B、CRC32 `2c877f19`、SHA256 `975f1d0ee256d7182d7a261400d3d2deef60c6007eea0e20c053aa4fef374a2d`。原始日志`build/player-mips10-formal.log`的SHA256为`bd3b241c4171a9be53a183c6ddbabd750604dde5569b04a63700fd637a370b76`；AMP原始日志及散列见上一节。以上CPU值和零错误边界已直接按两份原始CSV及真实`mips10`/`amp30`标签核对。

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

用户按人工步骤检查媒体库中A/B连续短按、按住连发及释放停止，Start进入目录/媒体、播放暂停/继续、长按菜单、诊断浏览/返回及停止返回，明确回复“全部正常”。这轮确认覆盖实体输入、页面及暂停/菜单显示；手机录像及实际声光偏差数值验证已由用户取消。

人工操作后的只读查询为会话IDLE、音频inactive、pinned0、GUI error0、输入丢弃0，AMP error/rejected0。当前会话设置曾为10%，已在停止状态恢复为7%，未再启动播放；下一次START采用该值，源码和manifest默认同为7%。inactive音频状态保留上一已停止会话的音量10与96017样本统计，不作为当前播放参数。最后GUI位于诊断页、菜单关闭。证据为本地`build/player-final-manual-status-board.log`与`build/player-final-volume7-confirmed-board.log`。

## 2026-10-20：兼容性第1步——真实呈现时间戳

- MP4源保留有符号PTS/DTS及包时长；删除固定30fps、帧号/30一致性检查。解码器在分配图片时绑定时间戳，输出携带独立呈现序号；显示以PTS对比48kHz已消费样本。旧AVI/WAV从绝对有理数边界生成时间戳。此次仍保留Baseline/单参考/无CTTS门槛，Main/High/CABAC/B帧属于后续阶段。
- 源与demux主机回归分别覆盖29和21个真实文件，包括24、25、30000/1001fps及90kHz下2970/2970/3060 tick量化。逐包与FFprobe核对PTS/DTS/duration；真实会话对新增五种时间轴各执行三轮暂停/恢复/EOF/STOP，末帧完整且无持续掉帧；旧AVI/WAV/压缩音频生命周期通过。GUI/显示回归和17项真实ELF边界核验通过。
- 构建派生脚本修正NV12行步长硬编码，保留帧池按实际几何提供的stride，保证clean rebuild可重现。两核Debug/Release均通过。正式Release合包2,293,648B、CRC32 `18543de5`、SHA256 `24d3a4aec616ef2cce186cdf0b84284244bb778868689341099c55845a855e45`；Debug 2,418,736B、SHA256 `c43b620ec857c942a08d8d3cecd775c60d1858b04f8095b08b52200c9bf61b18`。
- 正式RAM上传长度和板端CRC验证通过，日志`build/player-pts-final-ramload-20261020.log`。嵌入33/34ms MP4以7%播放到ENDED：decode/display=30/30，drop/late=0/0，max_late=7000us；音频total/written/completed=48000，所有音频错误0。STOP后IDLE、inactive、pinned0。宽幅GUI继续持有CUSTOM表面，heap free34,314,560B不视为视频泄漏。
- `build/player-pts-quantized-trace-20261020.log`保留113条trace，DECODE/SUBMIT/LATCH各30条完整覆盖0..29，PTS依次0/33000/66000/100000…966000us，末帧duration34000us。首次在ENDED请求同步trace被`PROBE_BUSY`拒绝，未影响播放计数；随后按接口要求STOP再导出。初次预备RAM包未用于本节验收，正式证据只对应上述SHA。
- 真实TF既有横屏MP4另做暂停/恢复前缀回归：两次PAUSED状态均为100320样本、63解码/62显示，暂停期间无时钟推进；恢复后STOP回IDLE，7%音量不变。原始日志`build/player-pts-pause-board-20261020.log`。本节短测不替代旧版本30分钟结果；不更新Flash，也不宣称已验证手机录像中的实际音画偏差。
- 上述TF前缀回归在恢复后的集中UART查询/STOP阶段另记录drop1、late1、max_late33334us，音频错误仍0；不能将其描述成全程视频零掉帧。33/34ms完整短片的30/30零丢帧结论独立成立。用户交互/诊断集中输出的时序影响在后续B帧联合验证中继续检查。

## 2026-10-20：兼容性第2步——CABAC、Main/High及六参考帧

- 解码门槛扩展为Baseline/Main/High、Level<=4.0、8bit420逐行、最多六参考；仍拒绝CTTS/B重排及超出首版两种尺寸。线性帧池扩大到九槽（六参考+当前解码+两显示持有），trace holds扩为64bit，保持CODEC/READY/PENDING/SCANOUT所有权。
- 派生路径统一使用完整、一次去转义的RBSP，去掉32字节slice header副本与非VCL第二次去转义；CABAC按字节对齐后进行物理字对齐。真实MIPS编译器定义`__CHAR_UNSIGNED__=1`，SDK CABAC m/n表中的负参数必须显式signed char；只修正这两张表，不全局改变char语义。
- 初始未修signed char的RAM包SHA `ecc086a33d0a1e0a559eb429f6a63ee5798639715f2676c1f042dc4f9c47e33d`在Baseline90帧像素正确后、首个Main/CABAC处失败；原始日志`build/player-cabac-suite-crc-board-20261020.log`与包保留于`build/player-evidence-cabac-r1`。新校验器正确拒绝此失败记录。初次采集工具未识别CRLF结束标志而等待，已修正；不将该失败计为CABAC通过。
- 主机使用真实派生硬件API生成VDMA，强制unsigned plain-char，核对I/P/B三种slice、三种PB模型、QP0/26/51的45个固定context golden words及21个termination context；与RBSP字节/释放测试合计CTest2/2。源29真实文件、七个profile/ref文件独立打开/规范化/音频解码、九槽边界、真实会话、GUI/显示回归及两核Debug/Release均通过。
- `make-player-compat-fixtures.py`生成七份3秒片源和一份21秒630帧诊断suite：Baseline/CAVLC/ref1，Main/CABAC/ref1/3/6，High/CABAC/ref1/3/6（8x8变换）。suite在关闭GOP的IDR处保留in-band SPS/PPS切换，PC完整NV12输出逐帧等于七段独立解码结果；这是同一文件的七段特性验证，不冒充七次独立板端打开。
- 修正后正式RAM包2,068,880B、CRC32 `680aa515`、SHA256 `1b0c96198c05e179448e42cd78228e69885a3e3a0a16f6d4368c956fc2a26e9f`。`build/player-cabac-signed-ramload-20261020.log`确认上传长度和板端CRC；不更新Flash。RISC-V音频镜像维持35880B/CRC `003d092f`。
- `run-player-codec-board.py`只在IDLE/inactive运行纯硬解CRC probe；GUI的DPU/旋转/ARGB缓冲继续独立持有。板端630帧、1260个Y/UV平面CRC与PC全部相同，PTS递增、EOF排空和STOP均成功；诊断解码累计3,210,043us，采样最小空闲堆19,305,496B，回收后45,596,504B。该CRC慢速同步探针不作实时30fps结论。日志`build/player-cabac-signed-suite-crc-board-20261020.log`，SHA `be5ed60516cd181bcc2a7fb0da21eb086659532297328a5f06e2b5e49b816b94`；JSON报告保留PC参考SHA和逐项判定。
- 同一包随后以正常GUI、AMP、7%音量完整播放21秒suite：ENDED decode/display=630/630，drop/late=0/0，max_late=13334us；audio total/written/completed=1008000，欠载/DMA错误/超时/队列/FIFO错误均0，GUI44次render/present、error0。六参考尾段空闲堆4,546,760B；STOP后IDLE/inactive/pinned0、空闲34,535,744B（wide GUI仍持有三张CUSTOM表面）。这是结束时观测值，不宣称分配器瞬时最小值。日志`build/player-cabac-signed-gui-play-board-20261020.log`。

原始`test.mp4`仍需第3步的CTTS、非零video edit和B帧重排才能直接打开；上述无B片源通过不等于原文件已验收。不沿用旧AVI30分钟结果作为新功能长跑，也不宣称测得实际声光偏差。

## 2026-10-20：兼容性第3步——B帧、编辑窗口和固定DPB工作区

- MP4接通CTTS、有符号DTS/PTS和非零video edit；按实际呈现窗口裁剪，不丢弃参考链需要的preroll。SDK恢复POC重排等待，明确限制重排深度4、DPB6，EOF排空。最后呈现帧按最大有效PTS判定，不按最后输入AU判定。音频早结束时补齐视频时间轴的静音，补齐数与DMA尾填充分别记录。
- 主机MP4为21个真实案例、7个拒绝案例；统一source为36个真实文件，含原始test.mp4。生产会话对Main/B2、High/B4、signed CTTS、preroll、尾裁、正首PTS和短音频各做三轮暂停/恢复/EOF/STOP；GUI、显示持有、按键与诊断行实测通过。真实派生codec的5项CTest覆盖RBSP、CABAC、POC/EOF、权重及固定工作区；实际参考表函数另验证I/P/B只访问启用条目，活动参考缺失明确失败。
- R1候选SHA `54ceeab1f2cc40474058997c25018a94ba7237d87d3e6102d68e192fc450a35f`对受控High/ref6/B4量化片99帧的198个Y/UV CRC和真实PTS全部正确，正常有声播放99/99、158400样本、音频错误0。原始文件994帧全部输出且Y/顺序/PTS正确，但呈现888–899的12个UV CRC不同；普通GUI播放另在AU215申请921600B的内部Y缓冲失败。原始失败日志保留，不算完整原片像素或播放通过。
- 内部CTRL/MV/Tile Y/UV改为九槽固定工作区，一次分配16,853,760B、关闭释放，避免逐帧大块分配碎片。R3 SHA `9cb8292c6c89baabd471eaedc3cff6b2f7bec68862139592133ac3e47588f74e`首次接入后在`ff_h264_execute_decode_slices`异常，EPC `80640a5c`、badva4；反汇编确认读取了未启用list1条目的已清空指针。修复按list_count/ref_count填写MC与collocated表，禁用项置零，有效项缺失明确报错，未恢复悬空指针。
- R4 Release 2,212,592B、CRC `c00d4fa7`、SHA `28fd79528ee921f24bf76765152d824acb238c1ab4f25a3ba7d88ebda9dbd83c`，Debug 2,341,264B，两核构建和地址核验通过；RAM日志`build/player-bframes-active-ref-ramload-20261020.log`。正常GUI/AMP/7%完整播放原始TF test.mp4：994解码、989呈现、drop/late5/5、max_late67000us；有效音频1592520个样本全部完成，所有音频错误0。ENDED空闲426264B，STOP后34388288B、IDLE/inactive/pinned0；工作区peak8、active0。此内存是状态查询观测值，不称瞬时峰值。
- 同一R4另做暂停/恢复与菜单诊断回归：两次PAUSED查询都为275040样本、176解码/169呈现，时钟与队列持有未推进；恢复到ENDED为994解码/992呈现、drop/late2/2、max_late57000us，音频1592520且错误0。自动三键菜单/诊断周期1/1完成、aborted0、GUI error0。最后LATCH的输入AU是987、PTS33099333us，最后输入为993，B尾段正确排空；STOP后堆再次34388288B。日志`build/player-bframes-active-ref-pause-ui-board-20261020.log`。这些实测不是零掉帧结论，也不代替新版本30分钟长跑。

### 已确认的MCE权重精度与用户选择

标准默认权重`1<<7`为+128，MCE的显式权重接口只有有符号8位。直接写0x80的R2/R4在同一12帧产生负权重/饱和错误：首次受影响P帧每个UV分量约少4096、各出现32个零值，PC对应无零值。R4的150帧GOP探针重复了这个问题；它未被记成像素验收通过。

PC隔离分析把5个P slice（原AU886/890/894/895/899）的默认128改成显式127/偏移0，CABAC图像载荷逐字节不变、重新对齐并去/加防竞争字节，FFmpeg trace确认实际se(v)=127。全部150帧Y/UV CRC精确复现R1板端结果。相对规范原片，仅呈现888–899有变化，累计4139个UV字节、最大差3；理想RGB换算最大差601full为5、709limited为7。分析JSON绑定GOP SHA `c354440fb3aebf4b67950d1d6de1ef26a7109727c676c661f6db3c683875e576`与R1串口日志SHA `f8a0c9eae0163027b0c93cb31d645ebbcc197fe0186e9d644f4afa3861166347`。这不是实屏RGB测量或其他文件的误差上限。

用户明确选择“兼容优先：允许这一小段色度近似，并在诊断页标明”。最终解析器保留规范128，在硬件边界仅对启用的显式加权通道/参考把+128映射为127，负128不改；成功提交的同一AU只计一次，GUI显示“权重近似”，串口导出`weight_approx`。实际parser/VDMA/hook回归覆盖活动边界、负权重、多slice和失败AU不累计；真实GUI断言诊断行显示5帧。

本阶段最终R5 Release 2,212,960B、CRC `5d560034`、SHA `37f9e27c8bf30bb81ae0fec0e0490952dc183cd461db450cdd5356b9f17a6132`；Debug 2,341,632B、CRC `47684d76`、SHA `6334a6972baa5bfde46d201567abc3522318e43c6dadf4fc1f80471e0effe8b9`。两核Debug/Release、主机会话和GUI已通过。全部新版本仅RAM运行，Flash保持此前版本。

R5已完成独立板测，RAM日志`build/player-bframes-approved-ramload-20261020.log`确认长度与CRC。`build/player-bframes-approved-gop-crc-board-20261020.log/.json`中的150帧、300个Y/UV平面全部匹配明确标记的PC“显式127权重”参考，顺序/PTS正确；这是批准的近似参考，不是原片规范像素完全一致。随后正常GUI、AMP、7%完整播放原始test.mp4：994解码、987呈现、drop/late7/7、max_late67000us，音频1592520样本且所有音频错误0；`weight_approx=5`与实际5个P帧一致。ENDED堆426256B，STOP后34388288B、IDLE/inactive/pinned0，工作区peak8、active0。日志`build/player-bframes-approved-original-play-board-20261020.log`。显示帧数和迟到数照实保留，未将功能播放成功等同于零掉帧。

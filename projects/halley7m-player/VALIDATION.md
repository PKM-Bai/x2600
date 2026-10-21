# 播放器实施验收记录

## 2026-10-20：R9更新Flash与自动启动检查

将`7e7d61a`对应且用户已目视确认的冻结R9安装到原NOR应用范围。紧凑包
1951392B/CRC `d4131fe7`原样嵌入，追加375136B FF形成2326528B/CRC `3d7e43a0`，
SHA256 `52e79b4d8dddfd97cdc2073105efeeee5da03d92ab788e4cc9b88623dd88e83c`。
核验旧R8完整应用区与环境后擦写，全部2326528字节回读一致，环境及持久启动命令
通过；SPL/U-Boot代码CRC `3a3c3918`、rootfs CRC `8618c73f`保持不变。

写入后由NOR自动加载，独立查询确认初始IDLE、媒体库、音频inactive及默认7%。
吉他MP4首段`12960/0`有效样本时已有115750816us时长，随后控制条隐藏；暂停后
控制条常显、时长不变，音频欠载/DMA/队列/FIFO错误均0。短播134帧解码、130呈现、
drop/late4/4、max_late61000us，如实记录；STOP后资源pinned0及音频inactive。
再次软件复位从NOR启动后停在媒体库，双核active、GUI1/1、错误0，音频关闭。
R9本轮未做物理断电冷启，安装JSON保留`cold_boot_verified=false`。
安装身份、产物路径和下一次previous参数见[NOR安装说明](NOR_INSTALL.md)。

| 本地证据（`build/player-ui-fixes-20261020`） | SHA256 |
| --- | --- |
| `flash-install.log` | `b8cd01121c6e3b91dce0dbbc57e103ff4617989dfec4e5a8c14b9b23aff77b2e` |
| `flash-install.json` | `b30a00f40776101f1633a772a928137b5a49c43bbb5244aed16c35e349ebcf60` |
| `nor-smoke-board.log` | `4a4254dc8c03e84301cbdc3bf7cba873c8c4274717f59e9fbf882e5f4d48019e` |
| `nor-smoke-board.json` | `9486f291b5c38b99c97284b62954e2b380ef556fef22d3e98e2c914a6b69e702` |

## 2026-10-20：R9播放控制条隐藏与提前总时长（RAM验证）

修复播放页缺少空闲隐藏逻辑：仅视频PLAYING时约3秒无按键隐藏上下栏、进度条和
中央媒体提示，任意已去抖按键按下即唤出，仍执行原音量/暂停/长按菜单动作。
按住、暂停、菜单、ENDED、错误及资源持有时常显，纯音频不隐藏。输入扫描只记录
活动，LVGL更新仍只在GUI任务；可见性变化沿用原异步双表面和脏区同步通路。

原总时长误用音频协议`total_samples`，压缩解码在EOF前刻意保持0，故长期显示
`--:--`。新增独立的`duration_us`和`has_video`：打开时从完整流/编辑后媒体时间线
计算时长，取音视频终点较长者；MP4/VFR/B帧不靠帧率猜测，也不提前承诺实际PCM
样本数。已知时长在暂停和EOF保持稳定，未知裸音频EOF才补齐，STOP/新会话清零。
GUI总时长和进度使用媒体时长，自然结束进度满格；音频START/EOF/DRAIN契约不变。

主机验证为完整生产会话回归和headless LVGL回归。前者覆盖WAV/AVI、MP3/AAC/
M4A/MP4、VFR、24/25/29.97/30fps、B帧/CTTS/edit及音频短尾；新增LOADING阶段
MP4时长已知且音频总样本仍0的断言。GUI为79次异步表面提交，native/wide布局
均检查整表面alpha清零、多次隐藏唤出无残影、3秒边界、Start按下与释放分离、
队列背压、连发、计时回绕、暂停/菜单/EOF等常显及末尾进度。既有UI序列回归通过。

两核Debug/Release构建及内存/合包核验通过，Release1951392B、CRC `d4131fe7`、
SHA256 `17aae0405f08bc2864585b61de0699220fd83f33c7016c6b0ff8471a115c869f`；
Debug2083648B、CRC `f96f4d0d`，SHA256
`651dc893ba52e29e87d3eb3723a35469b45330d9a55591788926cec1f748664e`。
冻结ELF/BIN/MAP及修正为冻结路径的manifest在`build/player-ui-fixes-20261020`。
RAM上传由U-Boot长度和CRC核验后执行，默认7%，本轮未改显示时钟或背光参数。

真实TF自动板测已通过：吉他MP4在LOADING即有115750816us，播放初期仍为
`480/0`实际音频样本；空闲隐藏、Start按下唤出早于长按菜单、菜单/诊断往返1/1、
暂停4秒冻结2200800样本、常显及恢复后再次隐藏均通过。原始test.mp4载入即有
33200725us，完整994帧解码/988呈现，drop/late6/6、max_late57000us，1592520
有效音频样本完整，ENDED控制条可见、时长不变。30分钟AVI仅短播验证开头即有
1800000000us及隐藏，不能算重跑30分钟；M4A开头即有30000000us且不自动隐藏。
所有查询的音频欠载、DMA错误/超时、队列/FIFO错误均0；各次STOP后IDLE、pinned0、
音频inactive。吉他中途停止为1507解码/1439呈现、drop/late67/67，AVI短播为134/
132、drop/late1/1，如实保留迟到跳帧，不将本轮UI修复描述成视频零掉帧优化。

用户已在RAM修正版确认吉他视频开头显示总时长01:55，Start播放后约3秒上下UI和
进度条全部隐藏，再短按Start能重新显示并暂停，回复“两项都正常”。当前以上结果
为RAM板测及人工确认，不能当作Flash更新结果。

| 本地证据 | SHA256 |
| --- | --- |
| `build/player-ui-session-duration-test.log` | `0de9291d467f10c58af4affcf8177987300f802d552bc3c5081db0613b5140b3` |
| `build/player-ui-fix-20261020/gui-host/test.log` | `d49906ad2cb82a3df4abe4c4f5884066acbf3d70cb8b4f2dc87e76bc9be76b63` |
| `build/player-ui-fixes-20261020/ram-upload.log` | `a81f9fe048fb7df2f1a4c602cad3e9f22b76e5d9f351a05fada5fc090239873c` |
| `build/player-ui-fixes-20261020/ram-ui-board.log` | `e45f4b676aff1d14e15ec56a2e8c8abfe52bcaf3df4b8d968ceaa51db15de79f` |
| 同名`.json` | `3139362393c68c0adee5e65fc55ed9de8752a6d509f36e30de276d8000cf7fc0` |

## 2026-10-20：R8 Flash冷启动补验与频闪排查

用户反馈从Flash启动后，未播放文件的媒体库也曾出现整屏亮暗闪烁。本轮保留
原R8 NOR镜像、启动环境、背光和扫描参数，没有重写Flash。先以原参数的RAM
诊断版进入媒体库，再软件复位直接启动原R8 Flash，用户均确认不闪。最后按要求
完全断电、等待约5秒再上电，用户回复“已断电重上电，没有频闪”。当前结论是
本轮未复现，原因未定位，不能记作频闪修复或长期稳定性通过。

冷启动日志捕获`sfcnor read`从`0x40000`读取`0x238000`到`0x80600000`成功，
随后跳转、PLAYER_BOOT、AMP_READY及DISPLAY/GUI/BUTTONS_INIT均成功；未中断
autoboot、上传镜像或发送Flash写命令。独立查询为AMP active、owner=riscv、
会话IDLE、未播放、音频inactive、默认音量7%，媒体库绘制/呈现1/1、GUI错误0。
USB串口中途断开后恢复，未捕获本次完整SPL/U-Boot输出及Reset status/PLL值；
物理断电操作由用户确认，不能宣称已有完整的冷暖启动PLL对照。

冷、暖启动快照的背光均为PWM15、PWMCDR=2、PWMCC15=5、PWMWC15=a6aa5356，
LPCDR均4000002f；DSI稳定时序配置一致。冷、暖媒体库EOD周期均为33010～33057us，
RAM诊断为33013～33055us。DSI错误状态00bc/00c0采样均0；这不是对整个运行期
错误累计的证明。R8普通`display`命令未打印显示UOT累计计数，也不能据瞬时
INT_FLAG为0推断从未显示欠载。PHY_STATUS的data-lane STOPSTATE位及GPIO输入
快照随采样时刻变化，未将其当成初始化配置差异。

最初抓到的运行快照已是吉他视频ENDED后的宽幅路线，不是未播放时的冷启动
故障现场，因此不能用于认定冷启动或横屏路线是根因。两条线索均保留待复现。
未采用的背光载波切换/VPG命令等实验代码及RAM产物保存在本地
`build/player-flicker-20261020`，已从功能源码撤回；未发布调参或修复版本。
若再次出现，应保留当时供电和页面状态，优先采集现场状态后再复位。

| 本地证据 | SHA256 |
| --- | --- |
| `build/player-flicker-20261020/flash-cold-boot-01-board.log` | `48c82406c27ed394f12e88e143090c866ec35d1b27eb2aedf40f46fef6ab2b22` |
| 同名`.json` | `00ce3b6294dcf2a2c35dbd3c9890b3c0f9608e022734c529f35bb7bd7fa2cf8c` |
| `build/player-flicker-20261020/flash-warm-boot-board.log` | `d0b7a615d1fafd7d1303a2165a3bddcdcd3d4b07eff77f1d8dc4ca28a2d4f58a` |
| `build/player-flicker-20261020/ram-1khz-baseline-board.log` | `ecefb7071ead1cf2356d563a68d2e17478dff04b7b26fd69574f6b34ff363a4b` |

## 2026-10-20：兼容性R8更新NOR并从Flash启动

用户明确要求将当前新版RAM固件写入Flash。使用已完成兼容性验收的冻结R8镜像，
对应功能源码`c0e5033`，未重新构建；紧凑包1949856B/CRC `be8b7739`、SHA256
`bad023ccbf951a25fec04e30ecbb08f356fa0a973ae867b6b56c845d5e6bba80`。沿用旧擦除范围，
追加376672B FF形成2326528B/`0x238000`的NOR包，CRC `b8c0bfad`、SHA256
`805c15d899246f81aa59a29ea623fd37ce1353d2e85db3f3c71aec9cf1090861`。真实ELF/BIN、
BSS填充边界、上次R4安装身份和双份原始16MiB备份通过；共享烧写流程及播放器包装
自测通过。本地准备使用独立输出目录，没有覆盖历史安装证据。

播放器STOP确认、音频关闭后，软件复位进入原U-Boot并上传。擦除前匹配整段6MiB
旧应用分区及有效环境，再擦写`[0x40000,0x278000)`；擦除FF核验、写入CRC及
全部2326528字节比较通过，环境CRC及持久bootcmd核验通过。SPL/U-Boot代码
CRC `3a3c3918`、当前rootfs CRC `8618c73f`前后不变。保持启动命令
`sfcnor read 0x40000 0x238000 0x80600000;go 0x80600000`及1秒等待。

reset后直接由NOR装载新版，未追加RAM上传或手工go；日志观察到PLAYER_BOOT、
AMP_READY及DISPLAY/GUI/BUTTONS_INIT=0。独立串口查询AMP active、owner=riscv、
会话IDLE、GUI媒体库页、音频关闭，默认音量7%。随后完整播放TF原始test.mp4：
解码994、呈现989、drop/late5/5、max_late67000us；1592520音频样本完整，
DMA错误/超时、队列/FIFO欠载及音频错误全0，权重近似计数5。GUI58/58呈现、错误0；
STOP后IDLE、kind0、pinned0、音频inactive，空闲堆34650432B、media栈最低余11264words。
如实保留迟到跳帧，未将RAM暂停回归的994/994零掉帧结果迁移到本次NOR运行。

安装记录为本地`build/flash-player-nor-compat-r8-20261020.log/.json`，原始播放记录为
`build/player-nor-compat-r8-boot-play-board-20261020.log`，逐份SHA及后续更新入口见
[NOR安装说明](NOR_INSTALL.md)。本轮确认写入、逐字节回读与软件复位自动启动，
`cold_boot_verified=false`保持如实；未进行物理断电冷启动或新格式30分钟重测。
用户可以停止后断电取卡、复制媒体，重新上电时由持久启动命令装载新版。

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

## 2026-10-20：兼容性第4步——常见尺寸、SAR与四组CSC

实现按coded矩形分配NV12及内部工作区、逐帧SPS格式快照、SAR等比居中和601/709有限/全范围CSC。格式按属性使用SPS已知值、MP4容器后备、显示推断；首个READY确认实际格式后才提交NV12或启动音频，等待GUI路线ACK时保留原token。中途格式变化明确退出。可见尺寸为正偶数、单边≤2032，分别向上对齐16后coded面积≤1280×720；任意非最小coded画布裁剪、1080p、HDR/BT.2020及ICC变换不在本轮范围。Level 4.1/4.2样片只改标签，未增加实际像素、参考帧或重排预算。

### 构建与主机验证

- R8双核Release为1,949,856B，CRC `be8b7739`，SHA256 `bad023ccbf951a25fec04e30ecbb08f356fa0a973ae867b6b56c845d5e6bba80`；Debug为2,081,600B，CRC `42a9d478`，SHA256 `ff5d925abb27e5b9bab6db0cf5101733cbd2f0e23035812612e150451fd3dad2`。小核仍为35880B/CRC003d092f、协议4/共享布局3，默认7%。两核Debug/Release链接及嵌入、边界、6MiB上传窗口核验通过，最终Release真实ELF/BIN及损坏副本17项检查通过。冻结产物位于本地`build/player-evidence-geometry-r8`。
- `test-player-geometry.py`的22个真实文件source/session用例通过，包含13个尺寸/色彩/标签文件、SPS优先于矛盾容器颜色/SAR、实际READY握手、正起始PTS、三轮暂停/恢复/EOF/STOP，以及奇数尺寸、1080p、coded对齐后超预算、HDR后备和中途格式变化的拒绝。codec/DPU为确定性peer，不计作硬件通过。完整生产session/core回归另行通过，包含旧AVI/WAV和B帧编辑窗口。
- 生产显示、旋转器、headless真实LVGL、按键及纯格式/逐属性resolve测试通过；实际codec提取回归5/5通过，包含RBSP、CABAC、POC重排、权重和工作区约束。独立`test-player-codec-format.py`编译实际SPS parser及生产helper，13个MP4元数据和同id0去VUI共14项通过，检查尺寸/crop/SAR/颜色/Level及旧字段清理；证据为本地`build/player-codec-format-host/{build.log,test.log,results.json}`。CUSTOM512采样测试核对列主序地址的独立固定值、全图双射及未完成/未持有表面拒绝。板测runner的7项假UART回归覆盖末行接收、CSC记录、安静播放窗口及同UI状态堆恢复；均不作为实板证明。
- 脚本及结果入口为本地`build/player-geometry-r8-production-host.log`、`player-geometry-r8-session-host.log`、`player-geometry-gui-host.log`、`player-geometry-display-sample-host.log`、`player-geometry-runner-host.log`、`player-geometry-r8-artifacts-host.log`。生成片源、Level重标、PC色彩参考和13例板测入口均已纳入源码；媒体、完整日志和工具链只保留本地。

### 候选实板证据与修正

- R6的854×480、640×360及960×540各90帧、180个Y/UV平面与PC参考CRC完全一致，分别验证coded864×480/stride896、640×368和960×544的padding裁剪，未把padding字节计入CRC。原始日志为`build/player-geometry-854-crc-board-20261020.log`、`player-geometry-640x360-crc-board-20261020.log`及`player-geometry-960x540-crc-board-20261020.log`。首个checker因参考JSON对象被当成列表而报数量不符；修正兼容`frames`数组后对同一原始日志核验通过，未重写板端结果。
- DPU CUSTOM实测为512字节块列主序`((x/128)*height+y)*512+(x%128)*4`。R6先用原始线性地址桥接读取854样片的16个色条/灰阶点，RGB与PC理想矩阵最大差1；初始两个布局猜测不匹配，保留为失败探测。日志`build/player-geometry-color-map-r2-board-20261020.log/.json`。R7采样命令采用已验证的布局，只读取ENDED后仍持有且没有写入的表面。
- `display`诊断打印较长，若读到`DISPLAY_FORMAT`就发长采样命令会溢出UART接收FIFO；R7增加`DISPLAY_END`，runner等待末行后再发命令。首轮320×240播放90/90且音频完整，但灰阶参考点靠近角标缩放边缘，PC点与DPU采样相位不同；PC参考移至平坦区并检查3×3邻域稳定性，RGB逐通道容差仍为2，未放宽阈值。原失败日志`build/player-geometry-final-board-20261020.log`保留。
- R7第二轮前六个横屏尺寸均90/90、drop/late0、144000音频样本完整且错误0；切到720×1280在open阶段返回ENOMEM，0帧解码。原因是旧宽幅GUI仍持有11,059,200B的三块CUSTOM时已开始分配新NV12/codec工作区。R8改为先完成异步route/GUI ACK、释放旧CUSTOM，再创建解码池；STOP或换WAV可取消尚未开codec的等待。主机回归明确断言路由等待期间只有IO池、没有大帧/codec分配。失败日志`build/player-geometry-final-r2-board-20261020.log/.json`不记为整批通过。

### R8最终实板结果

`build/player-geometry-r8-ramload-20261020.log`将上述冻结Release加载至RAM，板端长度/CRC校验及双核READY通过。随后完整13例使用同一固件、真实TF和正常GUI/AMP链路：

| 样片组 | 例数 | 解码/呈现帧数 | 有效音频样本 | drop/late | 音频错误 |
| --- | ---: | ---: | ---: | --- | --- |
| 七种可见尺寸及720×576/SAR16:15 | 8 | 720/720 | 1,152,000 | 0/0 | 全0 |
| 601limited、709limited、709full补充 | 3 | 270/270 | 432,000 | 0/0 | 全0 |
| Level4.1/4.2仅重标 | 2 | 180/180 | 288,000 | 0/0 | 全0 |
| 合计 | 13 | 1170/1170 | 1,872,000 | 0/0 | 全0 |

每例90帧、144000样本，EOF及STOP状态分别核对；会话IDLE、音频inactive、pinned0。四组CSC元数据和显示视口全部符合预期；12个横屏例共192个色条/灰阶/黑边写回像素，RGB对PC理想矩阵最大逐通道差1，要求容差2。原生720×1280无CUSTOM写回，只做格式/视口及帧音频计数检查，不伪称取得它的RGB样本。640×480及720×576/SAR16:15均为960×720视口、左右160黑边；854×480为1280×718、y=1。横屏→native→横屏连续切换成功。

13例ENDED查询的最小空闲堆898152B、media栈累计最低余12794words、GUI2934words，属于查询边界观测，不是分配器瞬时峰值。横屏STOP后空闲堆34650432B；native为45711192B，差11060760B为CUSTOM像素及对齐开销。各首次文件名状态在runner中标为`new-ui-baseline`，未把跨标题堆差当作严格泄漏断言；同画布且保留同文件名才作精确恢复比较。正式日志为本地`build/player-geometry-r8-board-20261020.log/.json`，原日志SHA256 `e3dc0caafa11609aa420ff916f364641b3f2780c7c595adc76239b8dbe128a8a`。该runner不查询板端SHA或TF文件SHA；预期固件/片源清单与独立RAM校验分别关联。

同一R8随后完整播放用户原始`0:/multimedia/video/test.mp4`，暂停时两次查询均137760样本、已解码90/呈现86，恢复后994/994帧、drop/late0、max_late27000us；1592520有效样本全部输出，DMA错误/超时、队列/FIFO欠载及会话错误全0。`weight_approx=5`，符合已批准策略。ENDED空闲堆687840B，STOP后34650432B、IDLE/inactive/pinned0，media栈最低余11264words。原始日志`build/player-geometry-r8-original-pause-board-20261020.log`，SHA256 `1f7c080cccaa1832418b7111db6b62462e07124c11096bdda9b605c81af9d564`。R5/R7的偶发跳帧记录仍保留，不能以本轮0改写历史或承诺所有视频均不掉帧。

再次播放SAR16:15样片并保留089末帧，用户人工确认“方向、比例和颜色均正常”；此为屏幕外观检查，不是光学色差或声光数值测量。最终STOP并刷新媒体库，会话IDLE、音频inactive、GUI错误0；串口刷新不改变当前播放页，结束状态日志`build/player-geometry-r8-final-idle-board-20261020.log`。板测始终保持7%音量，仅RAM加载；NOR保留此前已安装的R4版本。未将旧AVI的30分钟记录移用于本轮新格式，也不声明已测得实际声光偏差数值。

## 2026-10-21：GT911 与播放器触摸界面候选版

本轮范围仅为播放器，使用屏幕板载触摸接口，保留三键和 7% 默认音量。媒体库改为目录导航侧栏及十项文件窗口，播放页采用圆角浮动控制卡，诊断页和模态菜单统一配色。支持点选文件/目录、上级、前后十项、刷新、暂停/继续、结束重播、音量和停止返回；进度条仍只读。视频约 3 秒空闲后整层 UI 隐藏，隐藏后的首触只唤出，下一次点击才执行操作。

### 板级依据及实现

- 从本板两份一致的原始 NOR 备份提取 Linux DTB，检查 uImage 头和载荷 CRC。原 NOR SHA256 为 `5efd956157cf88fc8a55dd3695f75b216e184a9fa15bee00149680c740077926`，DTB 为 `e34b8a451acf216b6626273ea94d0c7481a9bff637adfdb205b0679192a1b57b`。节点确认 I2C0/100kHz、PC0 SCL/PC1 SDA Function 1、PC8 RST、PC7 INT、7-bit 地址 0x14，无独立电源 GPIO。证据和可复现提取脚本保留于本地 `build/player-touch-ui-20261021/board-evidence/`。未采用 x16xx 示例的不同引脚或 800×480 配置。
- 驱动读取产品、版本及当前配置尺寸，不上传配置表。原厂组的尺寸为 720×1280、最大触点字段为 10；协议支持最多十个有界记录，向 GUI 只提供一个跟踪 ID。正常轮询周期 10ms，错误释放并退避 100ms。I2C 单事务等待上限 6ms，大报告拆为最多两个 40 字节连续读取，全部读取后才 ACK；不使用音频 PDMA，不屏蔽中断等待 I2C。
- 触点始终在 GUI 任务转换为 1280×720 逻辑坐标，不随 native/wide 表面路线改变。控制命令异步提交，LVGL pointer 回调在最终表面持有检查之前执行，避免重新唤醒刷新后写入 DMA 持有的表面。
- 审查修正了复位延时的 tick 相位问题、诊断快照半更新、触点按住时目录代次/窗口/菜单变化导致旧目标被合成点击，以及 ENDED 时按钮误发 TOGGLE_PAUSE 无法重播。取消状态跨显示路线暂停保留；重播携带当前媒体完整路径。

### 主机与构建证据

- `python scripts/test-player-touch.py` 使用生产 C 协议、MSVC `/W4 /WX`，覆盖识别及两种地址、配置只读、READY/先读再清、释放、10 触点、跟踪 ID、越界、重复 ID、IO 错误、退避、方向及时间回绕。板级适配另由 XBurst2 R5.1.6 以 `-Wall -Wextra -Werror` 编译通过。
- `python scripts/test-player-gui.py --build build/player-touch-ui-host` 使用真实 LVGL 8.3.10、生产 GUI/input/surface/font 及 40ms 异步显示模型，完成 **223 次提交**。覆盖点选全局索引、命令背压、移出取消、上下文变化取消、route 暂停期间释放、native/wide 首触唤醒、长触不连发、暂停/重播/停止、进度不跳转，以及原有三键、alpha 清零、脏区同步和显示回归。六张 PNG 已检查；`test.log` SHA256 为 `615fc53a52887250ba9603d6164acb3215370a41fb9eaa82754fd6c4e74a436d`。截图渐变为主机合成背景。
- 生产 session/core 回归通过，包含原 AVI/WAV、压缩音频、MP4、时间戳和生命周期；本地日志 `build/player-touch-ui-20261021/session-host.log`。完整入口 `scripts/test-player-host.ps1` 已纳入 GT911，全部通过；日志 `build/player-touch-ui-20261021/host-regression.log` 同时包含输入、音视频/协议、存储、真实 LVGL、统计和操作序列回归。
- 双核 Release 紧凑包 **1,973,776B**，CRC32 `b20fc5e0`，SHA256 `963a722fd78a1244916f6050f98883a348ab22986ce3d73113cf27870ebeac96`；Debug **2,106,288B**，CRC32 `ed44ef07`，SHA256 `7a9cc3dd04f07e340ef2aa9e5f8109372260a28920342f75531db95c446fb686`。小核仍为 35,880B/CRC32 `003d092f`，协议 4/共享布局 3。两核入口、段边界、嵌入内容及 6MiB 上传窗口核验通过；冻结 Release 的真实 ELF/BIN 和损坏副本 **17 项**检查通过。
- 冻结产物、manifest 和构建日志位于本地 `build/player-touch-ui-20261021/{Release,Debug}` 及同级日志。NOR 派生包仅在电脑生成，2,326,528B/CRC32 `b1f03970`，未执行写入。

### 首轮 RAM 验证及 I2C 超时修正

初次提交时，Windows COM52/FTDI 驱动调用先阻塞，重插后原设备未枚举，未能进行 RAM 加载。用户随后恢复连接，首版以长度 1,973,776B、CRC32 `b20fc5e0` 校验通过并启动；日志为本地 `build/player-touch-ui-20261021/ramload.log`。双核 READY、显示及 GUI 初始化正常，GT911 读出地址 0x14、产品 911、固件 1060、720×1280、max_contacts 10，初始错误为 0。

首个真实 TF 的 854×480 短片解码/呈现 90/90、144000 个有效音频样本、drop/late 0，全部音频错误 0；STOP 后 IDLE、audio inactive、pinned 0。但触摸计数在 2540 次轮询时记录一次 ETIMEDOUT，自动验收明确失败并停止；后续空闲至 11480 次轮询，错误未增长。原始失败日志及 JSON 保留在本地 `build/player-touch-ui-20261021/ram-ui-board.*`，没有将视频成功等同于触摸验收通过。

代码审查确认旧循环每轮只收发一个字节且使用旧 STOP 快照，存在媒体任务抢占后数据已完成却先判超时的竞态。本轮改为有限批量填充 TX/排空 RX，全部命令入队后才开始 6ms 设备完成等待；入队另有独立 6ms 预算。接收后读取新 STOP/活动状态，超时临界点再复核一次，enable/disable 到期也最终重读 ENBST。未扩大优先级或 IRQ 临界区，未放宽完成限时。首次实板超时未保存底层寄存器快照，其具体触发阶段未被直接测得。

新增测试直接编译生产 `player_touch_board.c`，只替换 MMIO/时钟/RTOS 边界，模拟 12ms 抢占覆盖入队、已完成 RX、STOP 后到、deadline 采样及 enable 状态变化；真正的 TX/RX/enable 卡死、无 STOP、NACK 和溢出仍有界报错。`scripts/test-player-touch.py` 的协议及传输两项 MSVC `/W4 /WX` 测试通过，日志为本地 `build/player-touch-ui-20261021/r2/touch-host.log`。

修正版 R2 两核 Release 为 **1,974,032B**、CRC32 `b61a81ec`、SHA256 `3244d1ffa422beeb2cf3df1e6729b8022feed9badccfa9a5ebe93ded8fe63fa9`；Debug **2,106,544B**、CRC32 `7a7a84e2`、SHA256 `01f1615f59ef7c48a633fae26e282983d5c8ba00da61eec2744e9029d3248812`。构建与地址/嵌入核验通过，冻结 Release 的真实 ELF/BIN 及损坏副本 17 项通过，产物保留在本地 `build/player-touch-ui-20261021/r2/{Release,Debug}`。

R2 RAM 上传及校验通过，正常 GUI/AMP/7% 的真实 TF 复测中，854×480 和原生 720×1280 短片各 90/90 帧、144000 样本、drop/late 0，全部音频错误 0。吉他片短播及一次完整三键菜单/诊断序列通过，暂停前后两次查询均为 2203200 样本，恢复后隐藏正常；本轮停止时 1507 解码/1442 呈现、drop/late 65/65、max_late 121000us，照实保留。原始 test.mp4 全部 994 帧解码、988 呈现、drop/late 6/6，1592520 样本完整、音频错误 0、weight_approx 5。它们不构成零掉帧或新版本 30 分钟证明。

截至这些复测结束，GT911 9869 次轮询错误为 0；后续故障诊断期间至 66004 次轮询仍为 0。用户在黑屏时实际触碰，驱动收到 13 次按下/释放，坐标报告有效、invalid 0；未将这些未明确对应屏幕目标的触点记作方向或命中验收通过。记录为本地 `r2/coordinate-touch.*`。

R2 全套自动验收仍失败：上述 wide 文件切回原生 acceptance-30min.avi 后，解码 22 帧、呈现 19 帧、drop 1，发生显示/STOP 超时 -116、pinned 1。音频已关闭、音频错误仍 0、触摸仍正常；用户确认屏幕为黑屏。连续快照保留在 `r2/avi-display*.log`，显示 pending 事务与硬件旧 layer 地址长期不匹配，TFT_ST 持续为 0。该失败未计作完整播放器通过，显示运行中描述符提交正在单独修正。

当前 Flash 仍为 R9，新的 NOR 派生包仅在电脑生成。可见界面恢复后还需触摸方向/命中和人工操作验收；用户已取消手机录像和实际声光偏差数值验证。

### R3 后续：界面和点击确认，显示故障仍保留

R3 将正在扫描的 native composer 新描述符提交改为只写地址，重启及 wide one-shot
仍保留 START。用户随后明确确认新版界面画面正常、触摸点击准确；真实操作后的
GT911 记录为 47 次按下/释放、errors/invalid 均 0。这项确认补充上文首轮黑屏时
尚未完成的方向与命中检查，不代表所有播放生命周期均已通过。

同一 R3 的短片复测中，854×480 和 720×1280 均 90/90 帧；原始 test.mp4 为
994 解码/988 呈现、drop/late 6/6，1592520 有效样本完整、音频错误 0。用户操作
期间调整过音量，后段原片及 AVI 为 0%，不能把这部分声卡计数当作有声听感检查。
之后 wide 原片切到 acceptance-30min.avi 仍失败：22 解码/19 呈现、drop/late
1/1，显示及 STOP 超时 -116、pinned 1，音频 inactive 且音频错误仍 0。连续显示
快照的 vblanks 均停在 14202、mismatches 1，video/overlay 各有一笔未确认。
R3 的地址提交调整未解决该序列的全部问题，不能覆盖或删除 R2/R3 的失败记录。
原始证据为本地 `build/player-touch-ui-20261021/r3/ram-ui-board.log`、
`avi-after-ui-check.log` 和 `avi-display-stable-error.log`。

### R4：静止界面停止无效重绘，频闪尚未解决

R3 静止媒体库也会持续增加 GUI 绘制/提交计数；检查生产 LVGL 8.3.10 确认，
对已经可见的对象再次清除 HIDDEN 仍会 invalidation。GUI 增加幂等可见性更新，
列表、音频标识和播放控件只在隐藏位实际改变时操作；菜单背景色也只在实际不同
时写入。既有文本、进度宽度及背景 opacity 保留变化检查。该改动消除静止画面的
无效绘制与提交，不改变三键/触摸动作、3 秒隐藏、布局或默认 7% 音量。

生产 LVGL 主机回归新增固定画面但会话 revision 持续增加的测试：50 轮、共 5 秒，
覆盖媒体库、native/wide 暂停视频及固定音频界面，断言 rendered、presented、
提交数和前台像素 hash 不变，且无 pending。修正前媒体库断言已实际失败，修正后
完整 GUI/显示/按钮入口通过，共 154 次异步提交、六张 PNG；触摸取消、首触唤醒、
透明层清空、脏区、时间回绕和三键回归保持通过。主机日志为
`build/player-touch-ui-host/test.log`，SHA256
`5feda8d5c0c4c6359d60cf7f5bf12069dc609172e38ac44564d2bf6a0611429e`。
截图为主机合成显示结果，不能替代真实面板频闪检查。

R4 两核 Debug/Release 构建、入口/内存/嵌入核验及真实 ELF/BIN 和损坏副本 17 项
检查通过，冻结 ELF/BIN/MAP 与 manifest 在本地
`build/player-touch-ui-20261021/r4/{Release,Debug}`。Release 紧凑包
**1,974,368B**、CRC32 `3ea4ae8b`、SHA256
`5c045992257be35a9615b50e997bbdff3806d850bee08116e7284b67a7296d2c`；
Debug **2,106,880B**、CRC32 `6e36ea82`、SHA256
`e0f836ee0f318cafe98d5b671fd5419009588ffa32be75cd2a541d3e451e21a9`。
小核仍为 35,880B、CRC32 `003d092f`，协议 4/共享布局 3。

R4 RAM 上传校验通过，静止媒体库两次 GUI 查询之间包含 5 秒静置，rendered/
presented 始终 1/1；display 也仅一次 overlay 提交及确认、pending 0、mismatches/
underruns 0。GT911 计数从 8505 到 9107 次轮询、错误 0。该证据确认静止界面不再
反复向 DPU 提交，不包含 CPU 忙率实测，不能给出 CPU 使用率下降百分比。用户在
这一状态仍确认整屏亮暗频闪，因此本次幂等更新不能记作频闪修复，背光与面板问题
仍在单独定位。

R4 另完成两个仅约 8 秒的 native AVI 前缀复测，均默认 7%：启动后直接播放时，
查询为 242 解码/240 呈现，STOP 后 299 解码/298 呈现、drop/late 0/0、max_late
16667us、pinned 0、audio inactive；停止前已解码但未呈现的一帧不计为丢帧。
先播放 854×480 wide 短片为 90/90 帧、144000 样本完整，再切回 native AVI 时，
查询为 240 解码/238 呈现，STOP 后 298/298 帧、drop/late 0/0、max_late 23334us、
pinned 0、audio inactive。两轮查询的音频欠载、DMA 错误/超时和队列/FIFO 错误
均 0。它们证明这两个短序列通过，未重跑 R3 的完整 wide 原片及人工操作序列，
也不是新版 30 分钟验收，不能据此宣称旧超时在所有切换场景消失。

| 本地证据（`build/player-touch-ui-20261021`） | SHA256 |
| --- | --- |
| `r3/avi-after-ui-check.log` | `541e7e5341bd729262b4bbcb19e10853b7f6a2c113d623c12353efdef0c0d24c` |
| `r3/avi-display-stable-error.log` | `2c2fb904df16ecb8eb4d928f89e2a3aaaf86b2bd9028de530eae554ee24e7d48` |
| `r4/static-library.log` | `4c5b9e17b9d58ddd4450ee10c72d38ebcc11c340368704411ea828db1a43d8e7` |
| `r4/direct-native-avi.log` | `f368817d65b4a6322f456efe3ace5490edf88c8b7df2caae567167e975c73567` |
| `r4/wide-to-native-avi.log` | `0444fb9824226a2c0034fce857a321d68583936207b1ccd860867a3eeba00d1b` |

截至上述 R4 记录，Flash 继续保留已安装的 R9；R3 的 NOR 派生/准备结果
`execute=false`，未烧写。后续 R5 仅增加诊断入口并作 RAM 加载准备，不属于以上
R4 静止界面及 AVI 证据，验收结果须按实际固件身份另记。

### R5/R6：触摸轮询、PWM 载波及 DSI 彩条频闪对照

R5 曾在运行中直接打开 VPG，CLI 返回成功、VID_MODE_CFG=00013f02，但用户看到
黑屏，未看到彩条；DSI 00c0=00000080。它是失败的诊断，不能作为彩条显示正常或
排除面板/背光的证据。原始 `r5/static-colorbar.log`、`colorbar-registers.log` 和
恢复正常模式的记录继续保留，本节不以 R6 的结果改写该失败。

R6 使用冻结 Release 紧凑包 **1,975,520B**、CRC32 `22ec2ba1`、SHA256
`9b44855f17212807f6041f35cb71af6b9041665975df6736fba3a12f15bfac6d`，
manifest SHA256 为 `6f1a83ce2691bf852d2613ebba5384dfd8f01fa07216d10e68cc31687b18408b`。
两核真实 ELF/BIN 及损坏副本 17 项检查通过，RAM 上传日志确认完整长度和板端 CRC
一致；仅 RAM 加载，Flash 继续保留 R9。以下证据均在本地
`build/player-touch-ui-20261021/r6`，不移用 R4 的短播放结果作为 R6 播放验收。

重新加载 R6 后停在媒体库、未启动媒体，session IDLE、音频 inactive、默认 7%。
GUI rendered/presented 1/1，DPU overlay 仅一次提交/确认、pending 0、mismatches/
underruns 0。原始 PWM 为 PWMEN=00008000、PWMCC15=00000005、PWMCDR=00000002、
PWMWC15=a6aa5356；LPCDR=4000002f。基线 EOD 周期 33013～33054us，DSI
0038=00003f02、00c0=0。用户确认这次新加载的静止媒体库仍整屏亮暗频闪。

随后逐项改变并收集人工观察：

| 对照 | 板内记录 | 用户观察及适用边界 |
| --- | --- | --- |
| 暂停触摸轮询 | `TOUCH_POLL enabled=0`；等待后两次 polls 都为 29812，GUI 仍 1/1，PWM 保持 a6aa5356 | 仍频闪。此操作只停止软件 I2C 轮询，没有关闭 GT911 电源或断开屏幕板载触摸硬件。 |
| 恢复轮询，再切名义 10kHz PWM | `enabled=1`、polls 恢复增加；载波命令 result=0，PWMWC15=10aa0856，PWMCC15/PWMCDR 和 LPCDR 不变，GUI 仍 1/1、DSI 00c0=0 | 仍闪，基本没有变化。只核对了寄存器和代码中的周期/近似相同占空比，没有用示波器实测背光引脚频率或电流，不能据此排除全部背光问题。 |
| 还原名义 1kHz，再用停止/复位序列打开 VPG | PWMWC15 恢复 a6aa5356；停止 DPU DMA、DSI PWR_UP=0、设置 VPG、PWR_UP=1 后重新启动；result=0，0038=00013f02，GUI 仍 1/1 | 用户明确确认“能看到彩条，仍频闪”。本轮有实际彩条观察，与 R5 黑屏诊断分开记录。 |
| 退出 VPG，恢复正常 GUI | 正常模式命令 result=0，0038 恢复 00003f02，PWM 保持 a6aa5356；GUI 1/1、pending/mismatches/underruns 均 0，触摸恢复至 polls 53576、errors 0 | 已恢复正常媒体库和原 PWM，继续定位频闪，没有将诊断配置留作生产默认。 |

VPG 切换后的 DSI 00c0 为 00000080，恢复普通 GUI 后该状态仍为 00000080，照实
保留；基线、暂停触摸与 10kHz 对照时均为 0。该切换期 DSI 状态不等于 GUI
output_error，GUI 始终为 0，也不能据此把正常 GUI 的频闪直接归因于同一错误。
累计 EOD 周期在切 VPG 后最大 37999us、恢复后最大 59369us，包含模式切换过程，
不作为普通静止画面的稳定刷新周期。没有重置或删掉这些切换期观测。

截至本节，静止 GUI 没有持续提交、暂停软件触摸轮询后仍闪、实际 VPG 彩条下也
仍闪，这些都是各自条件下的观察；尚未定位根因，不能宣称完全排除触摸硬件、
背光或供电。用户说明开发板由 USB Hub 供电，下一步拟改电脑 USB 直连或独立 5V
供电作对照；该供电对照的实板结果尚未获得，不记作通过。
后续 COM52 的 `--wait-boot` 监听捕获新的启动，Reset status=00000001，未发送
reboot，并开始上传同一 R6；过程日志为 `r6/direct-usb-power-ramload.log`。
用户尚未文字确认具体改接方式，不能从这次启动推定已改为电脑直连；新的频闪
观察结果也尚未获得。

| R6 原始本地日志 | SHA256 |
| --- | --- |
| `baseline-library.log` | `2bb4a41efa4f9fb40f464ab629a13240fa7be74a361db1b4511252140cfd768c` |
| `touch-poll-paused.log` | `8ec215d7be3a993483a560ab8f86bb19122825e50343175cffa315afe65a832d` |
| `carrier-10khz.log` | `305cf5009aa8f7e6bd66ce195748a0d06b45ad75efbb35c4034d7211c82d8178` |
| `restore-carrier-and-vpg.log` | `15eefee701f87119b7103a419b871fcdcd37c275d25810de4e3a454292e6ffe5` |
| `restore-after-vpg.log` | `755ce139e404a7dacb3c6f0d27bb209e5204d6c0f9b748e4c905412ebb1a2080` |

### 独立供电与 R7：DC 背光对照、串口中断和实际固件身份

用户随后明确已改用独立 **5V/3A** 电源，仍有频闪。前述等待启动后的 R6 上传并未
完成：YMODEM 在 block 757 连续超时，期间又收到 Reset status=00000001，最终
达到重试上限，没有长度/CRC 校验成功或 go。开发板重新从原 Flash 启动 R9；
`r6/independent-power-flash-clean-status.log` 的 STOP 后查询为 IDLE、pinned 0、
音频 inactive、GUI 1/1。不能把这一轮供电观察记成同一 R6 的完整复测或 R6 已在
独立供电下持续运行。电源额定值来自用户说明，未实测板端电源轨。

R7 仅在临时诊断候选中增加 PC14 背光引脚 GPIO 常高/PWM 切换。冻结 Release
**1,975,648B**、CRC32 `42a9e2c2`、SHA256
`04a83a3c056f31209db5805d6abd3bc0609f980bb9f766729f76a66873ab8e40`，
manifest/17 项产物核验通过；`r7/ramload.log` 确认 RAM 长度与板端 CRC 后执行。
这次新加载的 R7 在独立 5V/3A、原名义 1kHz PWM 下，用户仍看到频闪。

DC 对照命令 result=0，PC14 配置从 GPIOC mask=7e003ff0、pat0=00000403 改为
mask=7e007ff0、pat0=00004403，pin 快照含 PC14 高电平；内部 PWMEN 仍为
00008000、PWMWC15=a6aa5356，改变的是引脚复用为 GPIO 输出，不是关闭 PWM
计数器。用户确认 DC 常高时仍频闪、基本未变。随后恢复 PWM，mask/pat0 回到
7e003ff0/00000403，波形寄存器仍为 a6aa5356。三次查询均 GUI 1/1、pending 0、
mismatches/underruns 0，基线 EOD 33012～33055us；恢复后的累计范围为
33011～33056us。该对照未实测背光电流或面板电压，不将其写成全部背光/供电
硬件已被排除，也未改变生产默认背光设置。

R7 的 `full-sequence.json` 总状态为 **failed**：前两例 854×480、720×1280 短片
各 90/90 帧、144000 样本完整、drop/late 0，STOP 后 IDLE/unpinned。吉他片已开始
播放，随后在 UI 操作阶段发生 Windows `ClearCommError`/拒绝访问；日志还收到
新的 SPL/DDR 启动输出及 Reset status=00000001。自动 cleanup 的 WriteFile
同样失败，没有取得 STOP/音频关闭确认。JSON 的 cleanup false 表示未获确认，
不能当成板内音频实际仍在运行的证据。该结果不是完整功能序列通过，也没有本轮
DPU 超时的板端证据，不能把串口异常改写成显示 -116；原 R2/R3 超时另行保留。

串口随后从系统消失；用户确认 COM14 是无关 CH340。FTDI 直连重插后 COM52
恢复，但实际已运行原 Flash R9。`r7/serial-restored-stop.log` 的 STOP 后查询为
session 0/IDLE、pinned 0、音频 inactive、GUI 1/1；`touch` 仅返回不含触摸命令的
旧版帮助。这份恢复后的状态属于 R9，不能用于确认 R7 cleanup 或 R7 触摸状态。

### R8：移除临时诊断、二进制核对与原 Linux 命令表证据

R8 移除普通版本的触摸轮询暂停、PWM 载波/DC、VPG 等临时诊断入口和额外 DSI
状态项，保留 native live-ADDR 提交、显示统计及相应主机模型/测试。GT911 协议和
生产传输测试通过；`r8-host/test.log` 的显示/格式/rotator/三键/生产 LVGL 回归均
通过，GUI 154 次提交。两核 Debug/Release 构建及每组 17 项产物检查通过。

逐字节核对冻结 R8 与 R4 的对应合包，Release 和 Debug 均完全一致：Release
1,974,368B、CRC32 `3ea4ae8b`、SHA256
`5c045992257be35a9615b50e997bbdff3806d850bee08116e7284b67a7296d2c`；
Debug 2,106,880B、CRC32 `6e36ea82`、SHA256
`e0f836ee0f318cafe98d5b671fd5419009588ffa32be75cd2a541d3e451e21a9`。
因此可引用上文相同 R4 二进制的短窗口板测证据及其限制；这不是新增 R8 实板或
供电验收，也不能补全 R7 被串口中断的完整操作序列。

从两份一致的原 NOR 备份验证 uImage 头/载荷 CRC 并解压原 Linux 内核后，提取
到 202 项面板初始化长包表，与当前 panel_zc502.c 重打包的 30906 字节完全相同，
包含各项头部、有效载荷和零填充；两份表 SHA256 均为
`93cc673a2d4ce6ca7fe9374414ec300a51fc561a6ef0e7b27721063f2dc7edd3`。
报告为本地 `panel-command-evidence/report.json`。该比较仅覆盖这张命令表，不
证明 Linux 与当前程序的整个电源/复位延时、sleep/display 命令、控制流程或运行期
DCS 写入完全一致，也没有测得面板内部寄存器回读，不能据此宣布频闪原因。

截至本节，频闪仍未解决，Flash 一直保留 R9。原 Linux 仅 RAM、只读及 init shell
参考材料已准备并开始上传，尚未执行 bootm，不能提前记录其面板表现或稳定性结果。

| 本地证据（`build/player-touch-ui-20261021`） | SHA256 |
| --- | --- |
| `r6/direct-usb-power-ramload.log` | `4b915e85ab39a6d61b092f5da293a56e78a0a767f75f61ae8c00fa099b3ae654` |
| `r6/independent-power-flash-clean-status.log` | `b4e8783d2cb0104b553cbc22d5223dc1d9cac6fd26f26ba972973e7a947d2319` |
| `r7/baseline-independent-power.log` | `3f1d9ff5778773b5649c5442443c47b3375532086459a4bfd078a485b2d3e94d` |
| `r7/backlight-dc.log` | `573ee3e14089afe7c68f7137714978c5cc17bef2e9b61e969b3024f1c3d9958e` |
| `r7/backlight-pwm-restored.log` | `c76f0458256c6dec0927c20f97b1947bf464c56567778e2f57d45449f481bc48` |
| `r7/full-sequence.json` | `b98cf6adc893b9ced3a9b32e5a102e2609a23bbdcaf2f82e0cf7c9edd4bbabb5` |
| `r7/full-sequence.log` | `2dd5411d29d30cd59413d918b3b745528f07b2bef6c628c640a0264ada47c5f9` |
| `r7/serial-restored-stop.log` | `a69f39d97030b2ae78bed097fa4d260d908e7d016d70c3309fed9b80720dd09b` |
| `r8-host/test.log` | `7acdad325924e787604d871359932e9ee6afead4ec18cd9636f40d1b4ac170ff` |
| `r8-host/touch-test.log` | `ef0498a306eb89c12943a6e75b043cf4cfbe797620943eb614ea7c2219894628` |
| `panel-command-evidence/report.json` | `5911e3753a6e383791f1114cb4e85cc2148a4d6684122c67ddfdfccc939675f2` |

### 原 Linux RAM 实屏对照与 NOR 前后校验

原备份 Linux-5.10.186+ uImage 仅从 RAM 引导，镜像 5,900,298B、CRC32
`1354ec7f`、SHA256 `b2c3f9354979635453535e55d19701bcc7dd49be18aad47a78ab670a8b84e8d0`。
初次工具因 iminfo 不支持、第二次因 shell 提示符识别超时留下的 failed 报告保留；
随后 `linux-reference/resume2-report.json` 的实际 shell 查询确认 PID 1 为
`/bin/sh`、exe 为 `/bin/busybox`，根挂载为 `/dev/root / jffs2 ro,relatime`，
没有原厂 rcS/APP/softburn 用户进程。命令行虽带 mtdparts 的 ro 参数，三个 MTD
flags 实际均为 **0xc00**，仍含 MTD_WRITEABLE=0x400；根只读已确认，MTD API
只读分区保护未生效，两者不能混写成全部 NOR 接口只读。

实际 fb0 为 layer0、720×1280/30、32bpp、stride 2880、virtual_size 720,2560。
图案工具尝试写两页共 7,372,800B：首两段各 1,843,200B 完成，组成上半 0xff、
下半 0x80 的完整一页；后两段报 ENOSPC、PATTERN_EXIT=1，原报告保留 failed。
未把两页写入记作通过，0x80 含 alpha 也不作为校准灰度值。用户后续明确确认
看到了有效白/灰静态图，且仍整屏亮暗频闪。因此已有原 Linux 实际显示后的
频闪复现证据，根因仍未定位，不能按此前黑屏诊断或仅播放器故障处理。

离开 Linux、回到 U-Boot 后，`nor-after-20261021T111000.358506Z.json` 完成
独立整片读取和四项 CRC 比较：whole `9ee8a8f1`、boot `3a3c3918`、rootfs
`8618c73f`、R9 app `3d7e43a0` 均与引导前相同，status=complete_match、
flash_contents_same=true。此结论为完整 16MiB NOR 及列出区域的前后 CRC 一致，
没有执行擦写或 saveenv。随后生产 R8 合包已恢复到 RAM 并启动，长度 1,974,368B、
CRC32 `3ea4ae8b` 与上节冻结身份一致；Flash 仍是 R9。

### R8 首轮完整序列：TF 读 token 错误，清理成功

`r8/full-sequence.json` 首轮总状态为 failed。前两例 854×480、720×1280 短片各
90/90 帧、144000 样本完整、drop/late 0；吉他片在 UI 循环启动前收到
`SD_IO op=read stage=read-token sector=5732209 count=1 cmd=17 r1=00 token=08 result=-5`。
会话进入错误状态：19 解码/17 呈现、drop/late 1/1、29760 有效音频样本，
error=-5、pinned 0。本次故障有 TF 读取错误证据，不改记为旧 DPU -116。
失败后的 STOP 清理成功，回到 IDLE/unpinned、音频 inactive；查询的音频欠载、
DMA/队列/FIFO、GT911、显示 route/rotator/underrun 错误均 0。首轮失败仍保留，
不能因后续短观察未复现而改写为完整播放/UI 通过。

停止状态下对同一 38,782,620B 吉他文件只读前 8MiB，512 次 16KiB 读取成功，
result=0、eof/truncated=0；含 tick 让出的壁钟耗时 4,615,670us、1,817,419B/s，
纯 read 累计 4,257,032us、1,970,529B/s。该吞吐和成功范围只代表此 8MiB 前缀，
不代表整文件或已找到先前 token 错误原因。

`guitar-short-observation2` 随后约 5 秒未复现读取错误，STOP 后 149 解码/145
呈现、drop/late 4/4、max_late 67000us，pinned 0、音频 inactive、音频及触摸
错误均 0；迟到丢帧如实保留。首个 short-observation 的 marker 错误，以及
after-io 完整运行器预期初始 native=0、实际保留 wide=1 的条件不符，都在 PLAY
前退出，属于工具预置问题，报告保留且不作为新一次播放验收。调整运行器初态
条件后的 after-io2 完整序列结果见下一节。

| 精选本地证据（`build/player-touch-ui-20261021`） | SHA256 |
| --- | --- |
| `linux-reference/linux-final-observation.json` | `55a8a5400757ea66decd6b0aa4bdca23514774eafbecdf5a2bcd5d48ae0987f7` |
| `linux-reference/nor-after-20261021T111000.358506Z.json` | `0e48427ba76b41dc3f156af62feb27d73d459dc524ff0631b9ee2052f141bde7` |
| `r8/full-sequence.log` | `ed3aa5394935aaea608b2b5089e36f5914c5b680e140a65aaed89e4ed4dd9b7d` |
| `r8/storage-read.json` | `ec02c19f9da349a3029243cd768e7359b88d1c8bdf14684aaf80282638498bae` |
| `r8/guitar-short-observation2.json` | `aabdf9fa1cf311d2384277beeb67bbf71afcedddcef47ab1a2bf7b66b224bdd4` |

### R8 after-io2：第二次 CMD17 读取失败，完整序列未通过

修正初态路线约束后，`r8/after-io2/full-sequence.json` 再次记录 failed。两例
geometry 短片仍各 90/90 帧、144000 样本完整、drop/late 0。随后吉他片开始阶段
收到 `SD_IO failure=2 stage=command-r1 sector=5731262 cmd=17 r1=3f result=-5`，
与首轮 sector 5732209 的 read-token 错误发生在不同位置及阶段。会话为
1 解码/1 呈现、drop 0、samples 0/0、error=-5；吉他音频尚未启动，音频查询仍
保留上一会话的 inactive 统计。失败后的 STOP 清理成功，IDLE、pinned 0、音频
inactive，触摸及显示错误为 0。完整播放/UI 序列因 TF 读取错误未通过，不能只写
成尚未运行；两轮失败保留，TF 传输异常的原因仍未定位。

该轮 JSON SHA256 为 `ad1a592636b178a78442f45de6dd1f561e7ce72fc9fa57760f02e51330773f1d`；
同名原始日志 SHA256 为 `c6213c3439d834b9109f285b64bcecf25dc92a490b63c620d0fb47d1bbb63414`。

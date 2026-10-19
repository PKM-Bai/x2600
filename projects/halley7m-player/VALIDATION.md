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
| 只读存储 | 真MIPS编译与主机FatFs接口边界通过；无写卡符号、无HAL_DMA引用。实际TF吞吐仍待测 |
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

- 真实WAV文件播放、音频主时钟与实际声学输出延迟校准。
- 真实TF读速、2MiB预读及包池、完整AVI音画会话。
- LVGL三页、中文字体、透明GUI叠加、三键在播放期间的实际操作。
- 高运动片源720p30、30分钟有声播放、手机同步测量、长AVI/AVIX实板及单核/AMP负载对照。

当前视频诊断以主核时间调度，不代表音画同步通过；短片结果不替代长文件和30分钟验收。

## 2026-10-19：双核音频服务与实际低音量试听

- 协议/共享内存升级为v2，增加独立音频命令和事件环、媒体会话与命令序号。PCM位置单位为48kHz单声道样本；256KiB共享环容纳131,072样本。STOP确认后才清空共享队列；异常时静音、复位小核并保留资源，需整板复位恢复。
- 真实小核消费PCM并独占AIC、Codec、主PDMA。首次10%两秒500Hz短音完成96,000样本，用户听到但认为偏大；5%两次均完整完成但用户认为偏小；随后7%完成96,000样本，用户明确确认合适并设为默认。后续自动测试保持7%。百分比是Codec档位映射，并非线性声压。
- 在7%下，暂停测试于24,480样本冻结100ms，前后计数相同；恢复后96,000样本全部完成。另分别完成1、15、16、17、479、480、481、5,003样本的短尾测试，补零不计入媒体样本数。每次STOP=0、DMA错误/超时、队列欠载、FIFO欠载均为0，结束heap均为56,289,608B。
- 这些结果验证了共享PCM传输、实际发声、周期尾段和暂停冻结；不替代真实TF/WAV、长期稳定性或音画同步验收。原始日志保留本地`build/player-tone-volume7-board.log`和`build/player-audio-volume7-lifecycle-board.log`。
- 小核SDK的阻塞`ulTaskNotifyTake(..., timeout)`会在critical内yield后留下中断屏蔽，首个READY后任务不再前进。工程改用非阻塞通知获取和`vTaskDelay`，未改SDK源；重新RAM加载后READY、ACK、PING和上述音频实测通过。

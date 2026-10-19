# NV12 视频与 ARGB GUI 显示适配

复用已点亮的 `panel_zc502.c` 和固定 PHY 时序，DPU 原生扫描 720x1280。
layer0 为 NV12，固定 stride768、UV offset983040、容量1474560字节；
layer1 为 straight ARGB8888，stride720、容量3686400字节。
四层 Z 顺序为 `2/3/1/0`，GUI 最上层；即使禁用层也不能复用 Z 顺序。
真实屏幕黑屏曾由重复 Z 顺序引起，不能以 DPU 读取地址正常代替可见画面验收。

## 接线与所有权

父 CMake `add_subdirectory(display)` 并链接 `player_display`，编译
`common/player_video.c`。HAL 配置按顺序包含 `x2600_hal_lcd.h`、
`x2600_ll_mipi_dsi.h`、`x2600_hal_mipi_dsi.h`；runtime 提供时钟、缓存和 IRQ。

控制任务在 GUI/媒体开始前执行 `init/start`，在两者停止调用后才执行 `stop`。
媒体工作任务调用 `submit_frame/clear_video/poll_completion`；GUI 任务调用
`submit_overlay/poll_overlay_completion`。内部短临界区序列化描述符配置，
首帧背光延时在临界区外，只允许一个任务实际执行，其他完成消费者返回0等待。
这些入口均不支持从 ISR 调用。

`submit_frame` 返回0才由会话把 READY 转为 PENDING；返回 `-EAGAIN` 保留 READY
稍后重试。成功排队不是旧帧释放凭证。视频完成的 `has_latched=true` 才对新帧
调用 `player_video_display_latch`；旧帧的显示持有在该事件后退休，CODEC/DPB
持有仍由解码器独立管理。旧的先标 PENDING 再 submit 的板测调用同样可用，
但若提交失败必须撤销新增的 PENDING 持有。

GUI 生产者保留两个表面。`submit_overlay` 返回0后，提交表面与旧扫描表面都
必须保持只读，直到 `poll_overlay_completion` 返回相同非零序号；此时仅旧
表面可写，新扫描表面继续保持。最多接受一个等待中的 GUI 请求：当前视频
仍在硬件中时先排队，下个视频提交合并此 GUI。媒体 worker 通过
`player_display_set_video_active(bool)` 告知连续视频状态：仅 AVI PLAYING 时为 true，
此时 GUI 请求一律排队，GUI poll 不另占一次刷新，由下一个视频描述符合并；
LOADING、PAUSED、ENDED、STOP/error 时为 false，GUI poll 可在硬件空闲后发起
GUI-only 描述符。默认 false 保留独立 probe 行为，切换标志不会释放任何缓冲。
该策略消除了 GUI 在视频 EOD 后抢先提交 GUI-only、让下一视频再等一轮的窗口。

面板为 30Hz 设计，实际 37.5MHz / (930 × 1332) = 30.272Hz，约 33.03ms 一轮。
GUI 和视频分开占槽会直接影响 30fps 呈现。`get_stats`/显式 `print_status` 的
`DISPLAY_SCHED` 保留视频接受/忙返回、GUI 接受/排队/合并/独立提交计数，EOD
周期最小/最大以及视频提交至确认最大时长；没有逐帧 UART。周期包含所有真实
EOD（包括旧描述符未匹配），不是媒体帧率；提交至确认也不是声光同步校准值。

GUI-only 描述符复用当前 NV12，绝不产生视频完成或改变视频 token。
视频与 GUI 各有独立的一个完成槽，不允许相互消费或覆盖通知。
`clear_video` 异步禁用视频但保留 GUI，完成为 `has_latched=false`、
`has_retired=true`；工作任务消费该退休通知且确认池中无显示持有后才可释放视频。
真正退出时，只有 `stop` 返回0才可释放所有显示持有和 GUI 表面；超时必须 pin
并重试，不能重新 start。跨视频会话 token 仍由上层帧池核验。

## 完成与缓存证据

EOD ISR 只读/清寄存器、更新固定完成槽，不调用 FreeRTOS、日志、HAL 锁或帧池。
确认一个提交必须同时满足：

- `FRM_DES_READ` 首字等于提交帧描述符的自链物理地址。
- 对启用的视频层，`LAY0_DES_READ` 第三个字等于 Y 地址，`LAY0_SITE` 在其缓冲内。
- 对启用的 GUI 层，`LAY1_DES_READ` 第三个字等于 ARGB 地址，`LAY1_SITE` 在其缓冲内。

不匹配只增加 mismatch 计数并继续保留 pending 和旧 scanout。
撤层后禁用的视频不再要求 Y 地址匹配；新描述符自链确认后才退休旧视频。
这证明源和描述符已更新，不单独宣称画面已经被用户看见。

`prepare-player-display.py` 只写 build 中派生源码，SDK 原件不改。派生 HAL
保留 DSI/PHY 修正、使用 full-range BT.601 的 CSC_MODE_2，PREMULT 取每层
`domain_multi`；两个当前图层都为0。描述符启动前清理完整缓存行。
NV12 像素由 FELIX 生产者发布，显示驱动不清其缓存，防止旧 CPU cache 覆盖 DMA。
GUI 适配器只发布自身后台表面实际脏区，以及上次完成后同步产生的脏区。

## 已验证和待验收

阶段3静音视频已经实板验收：用户确认画面完整、方向和颜色正常；90帧跨3个GOP
逐帧Y/UV CRC与PC相同；稳态DPU30.272fps、late0/max_late27.38ms、欠载0、
mismatch0。首帧背光准备104ms在媒体时钟开始前完成，原始IRQ全区间与稳态指标
分别保留。这些证据不替代后续音画同步和30分钟连续播放验收。

新增覆盖层已通过 XBurst2 GCC7.2 目标静态库构建，driver启用
`-Wall -Wextra -Werror`。`scripts/test-player-gui.py` 对实际生产driver使用虚拟
HAL/RTOS/MMIO测试，覆盖错误EOD、GUI排队合并、GUI-only不伪造视频latch、
独立完成槽、撤层退休和stop超时pin，并复现 GUI 先于媒体 worker 在 EOD 后 poll
的竞争，断言 continuous-video 状态下不能增加 GUI-only 硬件提交，暂停/结束则
恢复独立更新。GUI模块另有真实LVGL渲染和四张截图验证。

整机 GUI 首次实板已由用户确认中文、方向和菜单正常；同步片首轮音频完成且
无欠载，但视频90帧仅显示63帧、丢27帧，尚未达到连续30fps。上述视频优先合并
修复通过目标编译和主机竞争回归后，实板同步片达到90帧全部显示、drop0/late0，
max_late6.666ms；29次GUI更新合入视频，video_busy0，EOD周期33.014..33.053ms，
最大提交至确认32.943ms，音频144000样本全部完成且错误为0。后续实体三键、暂停菜单及
页面往返已有用户人工确认，详见`../VALIDATION.md`；长时重复启停不从本次短片推定。
用户已取消手机录像及实际声光偏差数值验证，相关工具仅作可选诊断，尚无实际偏差数值结论。
主机测试不能代替真实面板时序。

## 普通横屏视频的硬件旋转路线

`player_display_set_video_geometry(width,height)` 由媒体 worker 在前一会话视频已
撤层并退休、下一会话首帧解码前调用。`720x1280` 保留旧 native direct 路线；
`1280x720` 使用 DPU CUSTOM 写回和独立 Rotator，TFT/DSI 仍为 native720x1280。
GUI 已接入可选的 mode/ack/attach 后端握手：返回 `-EAGAIN` 时 worker 后续循环
重试；GUI task先排空已提交表面并暂停刷新，驱动停稳旧DMA，再由GUI task清空
两表面、切换布局并全屏重绘。返回0才允许提交新视频。1秒握手超时或100ms停止
超时返回错误，保留可能被DMA持有的资源；不会从worker调用LVGL。

宽幅路线的 NV12 源为1280x720、stride1280，GUI为逻辑1280x720直排ARGB，
仍为straight alpha/PREMULT0。DPU将视频与GUI一起合成为CUSTOM格式，再由Rotator
旋转90度送显示端。不能将线性ARGB或NV12直接交给Rotator，也没有NV12输出地址。
3块512字节对齐的CUSTOM表面共11,059,200B，与解码帧池分开管理。
媒体STOP撤掉NV12后，若GUI仍使用宽幅路线，这3块表面继续属于显示输出；
切回native geometry或整体 `player_display_stop()` 后才释放，不能把该常驻量当作媒体泄漏。

寄存器依据为本地SDK所附 `projects/x2660-halley/Demos/printer/tpc/tmp/X2600_PM_layout.txt`
的第11、12章：DPU WB_Format=7、writeback启用/direct关闭、单次描述符；
Rotator基址0x13070000、RDMA_FMT=3、ROT_ANGLE=1、手动BUFF_CFG_ADDR提交。
SDK的 `HAL_COMP_Start` 会清除ROT_SEL，宽幅路线因此自行发布描述符并保持该位，
不修改SDK。旧native路线继续使用既有HAL启动。

EOW仅确认匹配描述符/地址的写回结束，FA仅表示取地址；观察目标CUSTOM的SOF及
地址后，还必须收到实际DPU EOD才产生原有 `has_latched/retired` 完成通知。
Rotator EOF独立结束读取持有；没有新帧时重投末表面，暂停/ENDED也能GUI-only更新，
不会增加视频计数。停止先停DPU生产，再等待Rotator ACK/idle后释放CUSTOM池。
`display`显式查询输出 `DISPLAY_ROUTE/ROT/WB`，运行中没有逐帧UART。

`scripts/test-player-gui.py` 已覆盖生产driver宽幅EOW/SOF/EOD区分、三槽生命周期、
重复扫描、GUI-only、旧路线回切与停止超时pin；真实LVGL覆盖异步暂停/布局ACK、
重绘及native/logical脏区同步，另输出 `playback-wide.png`。显示与GUI静态库均已
通过真实MIPS编译。**宽幅CUSTOM/Rotator尚未实板验收**，其SOF/EOD相位、方向、
连续刷新、30fps与暂停/结束页面更新需在宽幅片源上核验，不能由主机模型推定。

# 本地播放器 GUI

三页为媒体库、播放、诊断，Start 长按 800ms 打开或关闭快捷菜单。
A/B 首次按下执行一次，持有 350ms 后每 100ms 连发；Start 释放执行短按，
长按触发后释放不再触发短按；连续短按逐次执行。页面和菜单切换会清除旧上下文
事件，并要求持有按键先全部释放。没有触摸、蓝牙或双击模式。

媒体库只调用 `player_session_post` 和 `player_session_snapshot`，不读文件系统。
每页十项是完整目录的窗口，目录代次校验与工作任务当前选择共同决定激活条目，
避免快速移动后立即确认误用旧快照索引；文件数没有十项上限。
目录包含工作任务提供的 `..` 父项。命令队列满时保留当前命令重试，暂停消费后续
输入，重复事件不能挤掉一次性动作。播放页显示媒体名、状态、音频样本时钟与音量；
诊断页可滚动，并可用 Start 返回原页。

视频播放中约3秒无按键后，隐藏顶部信息、底部提示、进度条及中央媒体类型文字，
让视频叠加层完全透明。任意已去抖按键按下即可唤出，仍执行原来的按键动作；
Start不用等释放才唤出，但暂停短按仍在释放时触发。按住按键、暂停、打开菜单、
播放结束或有错误/资源持有提示时保持可见；纯音频界面不自动隐藏。
输入任务仅记录按键活动，全部LVGL可见性更新仍由GUI任务完成。

## 主工程接线

1. CMake `add_subdirectory(gui)`，链接 `player_gui`；它公开依赖 `player_lvgl`。
2. 控制任务先执行 `player_display_init/start`，之后保持显示器常驻。
3. 会话工作任务先完成 `player_session_init`，再启动 GUI；GUI 初始音量仅取
   会话快照，不设置初始硬件音量。GUI 任务执行
   `player_gui_init(player_gui_display_output())`，每约 5ms 调用
   `player_gui_step(now_ms)`。LVGL、字体暂存和全部页面操作仅属于这个任务。
4. GUI init 成功后，`buttons_set_sample_callback(player_gui_sample_keys)`，再
   `buttons_start()`。直接复用现有 buttons.c 唯一的 5ms 扫描任务/20ms独立去抖；
   退出扫描短临界区后回调，时间来自 `platform_time_us()/1000`。回调入口仅在短
   临界区操作动作队列，保证 GUI 绘图期间的完整短按也能保留；不调用 LVGL。
   stable mask 必须已完成 20ms 去抖；此入口本身不再去抖。按下为1：
   A=`0x001`（PE14/SW3），B=`0x002`（PE15/SW4），Start=`0x008`（PE13/SW6）。
   输入入口、GUI step、init/deinit 均为任务上下文，不能从 ISR 调用。
5. GUI 任务建议 4096 个 MIPS 栈字（16KiB），会话工作任务独立运行。
6. 会话停止用 `player_display_clear_video` 并等待退休完成，保留 GUI 图层。
   真正退出 GUI 前 `buttons_set_sample_callback(NULL)` 并停止 GUI step，确认 `player_display_stop` 成功，最后
   `player_gui_deinit(1)`；DMA 停止超时不能释放这两个表面。

逻辑坐标 1280x720；与受控视频相同，按顺时针 `(x,y)->(719-y,x)` 映射为
720x1280。LVGL 使用 32 行临时绘图缓冲，只旋转实际刷新矩形到当前后台 ARGB。
最后一块完成后暂停刷新定时器，提交整个后台表面；等待 DPU 的对应序号完成后，
才把本次改动的原生矩形同步到已退休表面，并恢复 LVGL 刷新。
同步写入的缓存区域与下次新绘区域一起发布到 DDR。前台及待确认表面始终只读。
由于 LVGL 的 invalidation 会自行恢复刷新定时器，每轮对象更新后还会重新检查
表面持有状态并暂停刷新，防止页间操作写到尚未确认的表面。

两个表面各 3,686,400 字节，绘图缓冲 163,840 字节，LVGL 固定堆 524,288 字节。
GUI 图层使用逐像素 straight ARGB8888、`domain_multi=0`，视频图层为 NV12。
播放器派生 DPU HAL 保留每层 PREMULT 配置，四层 Z 顺序必须唯一。
GUI-only 完成通知与视频 latch/退休通知分开，不改变视频池持有；暂停时也能刷新。

## 源码和验证

LVGL 8.3.10 直接使用固定 SDK checkout 中的 `components/lvgl/src`，不下载第三方
代码、不编译 demo/examples。`prepare-player-gui.py` 检查版本、记录 192 个源码
SHA256 和实际许可证位置。LVGL 原始 MIT 许可证见 `LVGL-LICENSE.txt`，此声明仅
覆盖该 LVGL 源码，不扩展到 SDK 的其他部分。

字体来自 ReGBA commit `7f379052dc14366e622a96c7cf7fb6b5a7e918a3` 的
`source/tools/unifont.hex.gz`，SHA256
`3d334e48426681bffe5005b4b399b704e8612fc162098d86383fe5463ae95c0e`。
生成 34,869 个原始 BMP 字形，运行时只在 128 字节暂存中将一个字形放大 2 倍；
没有打包整套放大字体。缺字显示替代方框。原字体许可为 GNU Unifont
(C) 2000 Roman Czyborra，GPL-2.0-or-later；完整来源声明保留于
`UNIFONT-LICENSE.txt`，字体适配器与数据沿用该许可。

`python scripts/test-player-gui.py` 用生产 LVGL/GUI/input/font/surface 模块执行
headless 测试，覆盖三键、上下文释放门控、命令背压、全局目录索引、旋转/边界、
异步表面持有、脏区同步、中文和替代字形、实际 straight-alpha 像素及三页菜单。
视频空闲测试在native和wide布局检查整表面alpha清零、反复隐藏/唤出无残影，
并验证3秒边界、Start按下立即唤出、按住/连发、32位计时回绕及暂停/菜单等例外。
四张 PNG 位于 `build/player-gui-host/screenshots`。截图中的渐变只是主机合成的
视频背景，不是板端画面。目标静态库和整机均由 MIPS 工具链实际编译；用户已
确认实板中文媒体库、方向和长按菜单正常。修正GUI抢占显示周期后，同步片90帧
全部呈现、音频144000样本完整且错误为0。后续实体三键、暂停菜单及页面往返已由
用户人工确认，见`../VALIDATION.md`；长期重复启停不从本次短片推定。用户已取消
手机录像及实际声光偏差数值验证，相关工具仅作可选诊断，尚无实际偏差数值结论。

同一脚本另编译实际 `display/player_display.c`，仅替换 HAL/RTOS/MMIO 边界，
验证旧 EOD 不释放、GUI 排队合并到视频、暂停时 GUI-only 更新、视频与 GUI
完成独立、撤层退休和 stop 超时保留。主机驱动测试不证明真实 DPU/DSI 时序。
该脚本同时运行原 `tests/buttons_test.c` 和新增回调测试，验证默认无回调时既有
轮询行为、去抖时间/独立按键、稳定电平、时间戳与注销回调。

普通1280x720横屏视频使用DPU整帧旋转时，GUI两表面改为逻辑1280x720直排，
LVGL坐标与表面字节数不变。后端 `mode/ack/attach` 回调在原GUI task内完成切换：
phase1排空提交并暂停绘制，phase2只在旧DMA停稳后清空表面、全屏重绘并ACK。
媒体worker重试 `player_display_set_video_geometry` 直到返回0；没有跨任务LVGL调用。
旧720x1280视频回到native表面CPU旋转方式。主机已覆盖两向切换及旧front持有，
宽幅 `playback-wide.png` 已检查中文/透明区域和布局；真实宽幅硬件显示仍待板测。

总时长和进度分母使用会话的 `duration_us`，与音频传输协议的 `total_samples`
分开。MP4在打开时即可按编辑后的音视频时间线显示时长，VFR和B帧不再依靠帧数
猜测。WAV/AVI以及有可靠时长元数据的音频也可提前显示；没有可靠总信息的裸音频
仍显示 `--:--`，EOF后用实际有效样本补齐。自然结束时进度满格，避免解码器priming/
尾裁造成最后一像素未满。播放页按media kind标识MP3/AAC；kind5仅在.m4a扩展名时
标为M4A音频。

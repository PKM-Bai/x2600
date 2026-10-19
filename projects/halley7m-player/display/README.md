# 播放器 NV12 显示适配

本目录提供阶段 3 的 DPU composer / MIPI 显示后端，没有实现 LVGL 页面。
复用当前仓库已经点亮屏幕的 `panel_zc502.c` 和 PHY 时序，通过派生源码接入
720×1280 原生竖屏扫描。电脑预旋转的横屏内容直接以 NV12 显示。

## 接入

MIPS 父工程添加 `add_subdirectory(display)`，链接 `player_display`，同时编译
`common/player_video.c`。MIPS HAL 配置需要按顺序包含
`x2600_hal_lcd.h`、`x2600_ll_mipi_dsi.h`、`x2600_hal_mipi_dsi.h`；
板级 runtime 提供 FreeRTOS、时钟、延时、缓存及 IRQ 支持。

所有公开接口由同一个媒体/显示任务调用：

1. `player_display_init()` 初始化控制器、面板和中断，保持背光关闭。
2. `player_display_start()` 允许提交，实际 DMA 在首个 submit 时启动。
3. 将帧槽标记为 PENDING 后，调用 `player_display_submit_frame()`。
   返回 0 表示已排队，**不表示旧帧可以释放**；`-EAGAIN` 表示上次提交或完成通知仍待处理。
4. `player_display_poll_completion()` 返回 1 后，核对 token/session，调用
   `player_video_display_latch()`。该操作将新帧变为 SCANOUT，撤销旧帧的
   显示持有；CODEC/DPB 持有仍由解码器管理。提交失败时由调用者撤销刚增加的
   PENDING 持有，不改变现有 SCANOUT。
5. 停止时调用 `player_display_stop()`。仅返回 0 后才可以撤销全部显示持有。
   若返回 `-ETIMEDOUT`，缓冲继续保留，不允许重新 start；可以重试 stop。
   解码器也停止后，才可调用 `player_video_pool_stop()` 进入新 session。

输入帧使用 128 字节对齐的 DDR cached/uncached alias，容量至少 1,474,560 字节；
固定 width=720、height=1280、stride=768、UV offset=983,040。layer 0 为 NV12，
CSC_MODE_2 对应首版 BT.601 full range 输出；layer 1 保持关闭。

## 完成判定和缓存归属

ISR 只处理 EOD 和欠载标志，不调用 FreeRTOS API、HAL 锁、日志或帧池操作。
**EOD 本身不足以回收旧帧**，还必须同时满足以下实际寄存器证据：

- 重置 debug descriptor 读计数后，`FRM_DES_READ` 第一个字为此次提交描述符的自链地址。
- `LAY0_DES_READ` 第三个字为此次提交的 Y 物理地址。
- `LAY0_SITE` 位于此次提交的 NV12 缓冲范围内。

不匹配时只增加 mismatch 计数，保留 pending 和旧 scanout，等待后续中断。
任何超时都不能作为自动释放依据。完成结构记录实际 descriptor、Y 地址、read site
和 IRQ 序号，供板测追踪。这里确认的是新源已被加载、旧源显示持有可退休；
不把 HAL 返回或 debug 地址单独解释为整帧已被人眼看到。

后端最多保存一个 pending 和一个完成通知。完成通知尚未读取时，新的 submit
返回 `-EAGAIN`，因此不会覆盖通知。读取时使用 FreeRTOS 临界区；首帧背光开启失败时
保留该通知，不悄悄丢失。成功 stop 会使本驱动不再产生旧通知，跨 session 的最终
token 核验仍由上层帧池执行。

`scripts/prepare-player-display.py` 仅写 build 下的派生源码，SDK 原件不修改。
派生代码保留已有 DSI / PHY 修正，设置 CSC_MODE_2，令 `domain_multi` 取每层配置
（默认 0），并在启动前发布当前 frame/layer 描述符的完整缓存行。
**像素缓存由生产者负责**，显示后端不清理 NV12 像素，避免用旧 CPU cache 覆盖
FELIX 已写出的 DMA 数据。

## 已验证与板测待办

已通过实际 XBurst2 GCC 7.2 的 mips32r2 / o32 / fp32 / little-endian 静态库构建，
新后端启用 `-Wall -Wextra -Werror`。SDK 驱动仍有原有格式与声明警告。
派生脚本通过 Python 语法检查，SDK 三份原始显示驱动没有改动。

**当前未板测**。必须先核对：

- NV12 色条、黑白阶与预旋转方向，排除 NV12/NV21、stride、CSC 和裁切错误。
- X2600 实际 debug descriptor 读口和 `LAY0_SITE` 语义，记录连续换帧时三项证据；
  新旧帧地址交替、提交发生于帧边界附近时，也必须正确保留/退休引用。
- 首帧、连续 30fps、欠载、停止重开及超时路径；stop 成功前绝不复用原缓冲。
- 结合真实 FELIX 输出验证缓存归属、画面撕裂、IRQ 时序与显示流水线延迟。

此静态库构建结果不代表 720p30、音画同步或 30 分钟连续播放已经验收。

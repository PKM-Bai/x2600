# AI 网络基础

当前已实现双网状态策略、lwIP 2.2.1 的原生 FreeRTOS 适配、固定依赖准备脚本，以及显式 DTRNG/MDIO 诊断入口。尚未实现 GMAC 数据帧和 SWT6621-SH 驱动接入，也没有实板网络收发或真实 TLS 握手结果。这里的 host 测试与 MIPS 编译结果不能替代这些验收。

## 依赖准备

在仓库根目录执行：

```powershell
python scripts/prepare-ai-network.py --output build/ai-network-deps --download
```

默认下载缓存为 `downloads/ai-network`；缓存齐备后去掉 `--download` 即可离线准备。也可以用 `--cache` 指向预先取得的相同发行包。脚本先校验固定 SHA256，再解包并核对源码版本和许可证，最终生成 `SOURCE.json`。正常固件构建不应自动访问网络。

| 依赖 | 固定来源 | 许可证 |
|---|---|---|
| lwIP 2.2.1 | 官方 GitHub 镜像的 `STABLE-2_2_1_RELEASE` 标签 ZIP | BSD-3-Clause |
| Mbed TLS 4.1.1 | 官方 `mbedtls-4.1.1.tar.bz2`，含 TF-PSA-Crypto 与 framework | Apache-2.0 OR GPL-2.0-or-later；附带组件保留各自许可证 |

完整发行包保留在缓存中。Mbed TLS 解包仅跳过 `tf-psa-crypto/drivers/pqcp/mldsa-native/examples/` 中的 Unix 符号链接；库的普通源文件、子模块与许可证均保留。AI 不构建该独立示例目录，不要求 Windows 创建符号链接的权限。

## 集成约定

- `ai_network.c` 无 RTOS 依赖，由一个网络任务独占状态。网卡事件更新 driver/link/address/cloud 状态，只有经实际验证云端可达的路径才成为 `selected`。Ethernet 优先于 Wi-Fi；两者都未验证时选 `NONE`。请求开始/完成时调用 `ai_network_set_request_active()`：健康 Wi-Fi 请求期间暂缓 Ethernet 抢占，结束后执行优先级切换；断链和传输失败仍立即切换。探测目标交替选择，避免坏以太网饿死 Wi-Fi；探测调度、超时及 socket 绑定由网络任务实现。
- `generation` 仅在已选择路径改变时递增。会话层据此中止旧连接的活动请求，保留输入供用户手动重试。认证、额度、云端业务错误保持网络选择；明确的网络传输失败清除该路径的可达标记。该模块不会重发任何请求。
- `sys_arch.c` 使用仓库现有 native FreeRTOS `queue/semphr/task` API。lwIP 堆栈尺寸按字节传入，转换为 FreeRTOS `StackType_t` 数量；毫秒等待向上取整，保留 0 为无限等待。ISR 仅使用 `sys_mbox_trypost_fromisr()` 与保护 API，不在 ISR 阻塞。
- 编译 include 需要本目录、依赖的 `lwip/src/include`、AI 实际的 `FreeRTOSConfig.h` 与 MIPS `portmacro.h` 所在目录、FreeRTOS kernel include、SDK core/soc-x2600/driver includes。使用 `gnu11`：SDK 的 `irqflags.h` 使用 GNU `asm`。此端口还依赖现有 `platform_time_us()`，提供 64 位单调时钟。
- 源码选择为 `lwip/src/core/*.c`、`core/ipv4/*.c`、`api/*.c`、`netif/ethernet.c`、`apps/sntp/sntp.c` 和本目录 `sys_arch.c`；不编译 SDK 内的 CMSIS `system/OS/sys_arch.c`。`lwipopts.h` 启用 `NO_SYS=0`、IPv4、DHCP/DNS、sockets/netif API、SNTP、软件校验和。所有实际 netif 操作需要遵守 tcpip 任务所有权。
- 在 `tcpip_init()` 前必须安装经实板验证的随机源。`ai_lwip_random_ready()` 只表明回调已安装，不能证明 DTRNG 已通过验收。当前仅有 DTRNG 诊断入口，没有验收过的持续熵服务，不安装任何固定种子或 tick 伪熵；缺少随机源时误启动依赖随机数的堆栈将明确触发断言。
- SNTP 使用 RAM UTC 基准，拒绝早于 2026-01-01 的样本，按 64 位单调时钟外推，超过 24 小时未校时则返回时间未就绪。默认 SNTP 更新周期 1 小时；服务器配置与启动由网络任务负责。SNTP 本身不等于可信时间认证，TLS 的 CA/hostname/date 校验不能关闭。
- Mbed TLS 当前只准备源码，未宣称完成 MIPS 配置/编译/链接或 TLS socket 适配。DTRNG 寄存器资料现已找到，仍需实板验证并实现持续熵服务、UTC 与证书链验证后才能进入实际握手验收。

## 显式实板诊断入口

`ai_board_rng_probe(256, &report)` 是有限样本诊断，读取手册指定的 DTRNG 寄存器并统计常值、相邻重复、1 比特数和变化比特，单次采样等待上限 10 ms，最多 1024 个样本。退出时恢复原配置及该模块门控；已有 RNG 消费者工作时返回 `-EBUSY`。结果不导出随机字作为密钥素材，不能将“有变化”解释为完成熵质量验收。后续需要重复启动、连续采样、错误路径和持续健康检测，才可启用 TLS 熵服务。

依据是本地 Ingenic `X2600_PM_layout.txt`：位于 `libbare-cpu/projects/x2660-halley/Demos/printer/tpc/tmp/`，章 41 页 1290～1294（寄存器基址 `0x10072000`、CFG/RANDOMNUM/STAT、位 12 清除 ready、位 11 中断屏蔽、位 0 使能），以及章 20 页 566 的 CLKGR0 bit 27。这份全文是在扩大检索时找到，替代先前“尚无 X2600 DTRNG 资料”的判断。

`ai_board_gmac_probe(&report)` 必须由显式命令调用。它暂时开启 MAC 时钟与 MPLL 分频的 50 MHz PHY 时钟，按 X2600 U-Boot `x2600m_halley7.h` 的 PC15 低有效复位及 PC15～PC24 function0 配置管理引脚，扫描全部 32 个 PHY 地址并读取 ID1/ID2/BMSR；BMSR 连读两次。每次 MDIO busy 等待上限 10 ms，退出恢复引脚和时钟；已有 MAC/DMA 接收发送时返回 `-EBUSY`。函数不启动帧 DMA、netif、DHCP 或网络请求。

MDIO 偏移 `0x10/0x14` 和分频 124 来自 X2600 手册章 36 页 1174～1176；表 4-9 页 67～68 明确 MAC `0x134B0000` 属于 AHB2，章 36.5 说明 MDC 从 MAC HCLK 分频。函数读取现有 PLL/总线分频，仅要求 AHB2 不超过 300 MHz，从而让 MDC 不超过 2.5 MHz。AHB0 保留报告，但不用于这个限制。非整数 PLL 或无法得到精确 50 MHz 时返回 `-ERANGE`，不猜时钟。

发现一项必须在帧收发前解决的资料差异：X2600 PM 章 20 页 530 把 `MACPHYCR.PHY_INTF` 的 RMII 值写为 4；本地 `x2600m_halley7.h` 则定义 2。当前 MDIO 探测保留此寄存器原值并回报 `phy_mode`，不擅自选值，也不据 PHY 可读宣称帧收发已经正常。

## 已执行检查

```powershell
python projects/halley7m-ai/network/tests/check_build.py --mips
```

Windows host 使用仓库现有 `msvc_environment()`，覆盖可达性优先、认证/额度错误不切网、网络失败切网、失链清除旧状态、双网轮流探测。MIPS 检查使用现有 XBurst2 GCC 与播放器验证过的 native FreeRTOS port/header，编译 44 个文件（本地 4 个、lwIP IPv4/API/SNTP 40 个），启用 `-Wall -Wextra -Werror`。它是编译检查，不启动 FreeRTOS 调度器、不连接网卡、不验证芯片中断/cache/DMA。

后续必须补充 GMAC 收发/PHY/插拔、SWT SH 适配、RTOS mailbox 实板负载、DHCP/DNS/TLS、双网切换与云端文字/音频连续运行验收。

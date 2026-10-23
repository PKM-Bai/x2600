# AI 网络基础

当前已实现双网状态策略、lwIP 2.2.1 原生 FreeRTOS 适配、GMAC轮询DMA驱动、持续DTRNG服务和以太网管理任务。RAM实板已完成100Mbps全双工、DHCP、DeepSeek域名解析与SNTP校时。SWT6621-SH驱动尚未接入。TLS适配已通过本机真实握手和MIPS编译，仍无板端TLS结果，见 [TLS模块](../tls/README.md)。

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
- 在 `tcpip_init()` 前初始化并安装 `board_entropy` 硬件随机源，供端口和事务ID使用。持续服务在真实DHCP/DNS过程中已成功供数；相邻字重复或超时会锁存故障，没有固定种子或tick回退。`ai_lwip_random_ready()`只说明已安装回调；TLS所需输出质量依据仍待补齐，详见 [持续随机服务](board_entropy.md)。
- SNTP 使用 RAM UTC 基准，拒绝早于 2026-01-01 的样本，按 64 位单调时钟外推，超过 24 小时未校时则返回时间未就绪。默认 SNTP 更新周期 1 小时；服务器配置与启动由网络任务负责。SNTP 本身不等于可信时间认证，TLS 的 CA/hostname/date 校验不能关闭。
- Mbed TLS已完成独立客户端适配与MIPS编译、可重定位链接；尚未绑定lwIP socket。DTRNG读取及UTC可用，但加密输出质量依据和生产CA接入仍未完成，板端握手尚未验收。

## 以太网任务与DMA

`ai_ethernet_start()` 在调度器启动后创建网络任务，它唯一操作GMAC、PHY及描述符。tcpip任务的`linkoutput`只将完整帧复制到8项发送队列；网络任务保留未接受的帧，遇到`-EAGAIN`稍后重试。接收帧复制到pbuf，再交给`tcpip_input`。所有netif配置通过`netifapi`进入tcpip任务，GUI只读取状态快照。

驱动使用8个RX和1个TX增强描述符，ATDS=1、DSL=0。DMA区首尾按128字节对齐，初始清理L1/L2后只通过KSEG1访问，OWN交接使用`sync`。TX未完成时不覆盖缓冲区；100ms超时记录故障。停止或重新配置前执行有界DMA软件复位，复位失败不释放硬件所有权。

当前板PHY地址0、ID`02430c54`，MAC版本`00002037`、HW_FEATURE=`490d0935`，增强描述符能力检查通过。驱动保留MACPHYC0模式2；已有DHCP/DNS/SNTP真实帧证明该模式适用于这块板。手册中模式4的差异仍保留，不扩展为其他板型的结论。当前MAC每次启动从DTRNG生成并设置本地管理单播位，后续产品版本需冻结稳定地址来源。

`net status`和`net phy`只打印网络任务缓存，不复位正在收发的PHY。界面区分启动、无链路、DHCP、DNS、校时及以太网就绪；就绪不表示云端已连接。诊断日志不打印配置中的密钥。

断链会立即清除地址就绪状态，每秒根据实际DHCP状态复查，覆盖同IP重新绑定。NTP域名由应用串行解析，并使用独立查询token和网络代次丢弃晚到结果；SNTP只接收解析后的IP。这样避免固定lwIP内部DNS回调在`sntp_stop()`后继续访问已关闭的UDP控制块。SNTP响应检查启用来源及originate时间戳匹配；同步前仅将单调时钟写入请求标识，不将其当作UTC。

`AI_ARP`记录收到的ARP数、指向本机的请求数、收到的回复数及已交给GMAC的回复数，另显示最后目标IP；不记录载荷。发送提交计数不等于完成，仍需结合`AI_GMAC tx`和错误计数判断。

`net ping`使用lwIP RAW socket，绑定当前地址，只向DHCP网关发送4个1472B payload的ICMP请求（IPv4总长1500B）。每包接收总截止时间1秒，逐包核对来源、ID、序号、校验和及全部数据；无关报文不会延长等待。网络代次变化时停止，结束关闭socket；不会直接访问GMAC或调用云API。

`net ping small`使用固定32B payload作对照。`AI_ARP gateway_mac`来自tcpip任务的ARP缓存；全零表示该次没有有效缓存，不能据此断言网关不存在。R4核对发现电脑和板端使用不同网关MAC，用户确认实际接在不同路由器的相同私网地址段；当前保持接线，电脑到板端的互访等待同网段条件。

`AI_MMC*`为GMAC owner读取的硬件统计，用于排查DMA描述符之外的MAC丢帧，但需要先确认计数在实际收发时有效。HW_FEATURE bit11有能力时才读取；控制寄存器ROR开启则累计每次读数，否则累计差值，复位时重建基线。状态打印不访问寄存器，也不改变接收错误帧、长度门限或PHY配置。大包Ping仍未通过，发送完成及DMA错误为零都不代替往返验证。

本板R5实测的限制：`AI_MMC available=1`仅表示硬件能力位声明支持；实际所有TX/RX计数在已确认的小包往返前后仍为0，尚不能作为有效观测。特别不能用其中零CRC或零overflow排除大帧被硬件丢弃。此差异需进一步资料或外部抓包确认。

## 显式实板诊断入口

`ai_board_rng_probe(256, &report)` 保留为停用持续服务时的有限样本诊断；当前控制台`net rng`改为读取持续服务状态，避免争用硬件。有限诊断统计常值、相邻重复、1比特数和变化比特，退出恢复原配置及门控；已有消费者时返回`-EBUSY`。这些统计不估算有效熵量，不能据此启用TLS。

依据是本地 Ingenic `X2600_PM_layout.txt`：位于 `libbare-cpu/projects/x2660-halley/Demos/printer/tpc/tmp/`，章 41 页 1290～1294（寄存器基址 `0x10072000`、CFG/RANDOMNUM/STAT、位 12 清除 ready、位 11 中断屏蔽、位 0 使能），以及章 20 页 566 的 CLKGR0 bit 27。这份全文是在扩大检索时找到，替代先前“尚无 X2600 DTRNG 资料”的判断。

`ai_board_gmac_probe(&report)` 必须由显式命令调用。它暂时开启 MAC 时钟与 MPLL 分频的 50 MHz PHY 时钟，按 X2600 U-Boot `x2600m_halley7.h` 的 PC15 低有效复位及 PC15～PC24 function0 配置管理引脚，扫描全部 32 个 PHY 地址并读取 ID1/ID2/BMSR；BMSR 连读两次。每次 MDIO busy 等待上限 10 ms，退出恢复引脚和时钟；已有 MAC/DMA 接收发送时返回 `-EBUSY`。函数不启动帧 DMA、netif、DHCP 或网络请求。

MDIO 偏移 `0x10/0x14` 和分频 124 来自 X2600 手册章 36 页 1174～1176；表 4-9 页 67～68 明确 MAC `0x134B0000` 属于 AHB2，章 36.5 说明 MDC 从 MAC HCLK 分频。函数读取现有 PLL/总线分频，仅要求 AHB2 不超过 300 MHz，从而让 MDC 不超过 2.5 MHz。AHB0 保留报告，但不用于这个限制。非整数 PLL 或无法得到精确 50 MHz 时返回 `-ERANGE`，不猜时钟。

资料差异：X2600 PM章20页530把`MACPHYCR.PHY_INTF`的RMII值写为4；本地`x2600m_halley7.h`定义2。当前板保留2并已经取得实际帧证据，完整日志及版本绑定见 [验证记录](../VALIDATION.md)。

## 已执行检查

```powershell
python projects/halley7m-ai/network/tests/check_build.py --mips
python projects/halley7m-ai/network/tests/check_entropy.py --mips
```

Windows host 使用仓库现有 `msvc_environment()`，覆盖可达性优先、认证/额度错误不切网、网络失败切网、失链清除旧状态、双网轮流探测。MIPS 检查使用现有 XBurst2 GCC 与播放器验证过的 native FreeRTOS port/header，编译 44 个文件（本地 4 个、lwIP IPv4/API/SNTP 40 个），启用 `-Wall -Wextra -Werror`。它是编译检查，不启动 FreeRTOS 调度器、不连接网卡、不验证芯片中断/cache/DMA。

后续必须补充连续大帧/插拔、SWT SH适配、RTOS mailbox实板负载、TLS、双网切换与云端文字/音频连续运行验收。SNTP服务器采用[阿里云公开地址](https://help.aliyun.com/en/eci/user-guide/configure-the-ntp-service)和[Cloudflare公开地址](https://developers.cloudflare.com/time-services/ntp/usage/)，时间就绪与TLS身份验证分别处理。

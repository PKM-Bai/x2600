# 持续硬件随机服务与 TLS 边界

`board_entropy.c/h` 为 lwIP 随机端口、事务 ID 和启动时的本地 MAC 地址生成提供硬件随机字节。它使用单个 FreeRTOS mutex 串行访问 DTRNG，保留跨调用的前一个字，遇到相邻 32 位字完全相同或硬件超时便锁存故障，并关闭 `gen_en`。没有固定种子、时钟种子或失败后的伪随机回退。

## 手册与 SDK 核查

依据是本地君正《X2600 IoT Application Processor Programming Manual》，文本在 `libbare-cpu/projects/x2660-halley/Demos/printer/tpc/tmp/X2600_PM_layout.txt`：

| 本地证据 | 支持的结论 |
|---|---|
| 章 41.1，页 1290，行 83101～83102 | 厂商描述用途包括系统加密种子。 |
| 章 41.3，页 1290，行 83114～83134 | 框图有随机生成器及 `Post processing` 模块。 |
| 章 41.4，页 1291～1293，行 83168～83300 | 基址 `0x10072000`，CFG/RANDOMNUM/STAT 偏移 0/4/8；十条 inverter enable、10 位分频、中断屏蔽及 ready clear 位。 |
| 章 41.5，页 1294，行 83335～83350 | 轮询 ready，读取 32 位字，再将 `rdy_clr` 置 1 后清 0。 |
| 章 20，页 566 | CLKGR0 bit 27 为 DTRNG 时钟门控。 |

章 41 没有给出后处理算法、后处理压缩率、实际输出的最小熵界限、工作条件下的质量参数或在线健康检测阈值。框图中的后处理名称、用途描述以及有限实板样本“有变化”都不能直接推导出每输出字节具有 8 位熵。

对 SDK 的 `drivers-x2600`、相关 DTRNG 文件名及 U-Boot XBurst2 目录进行了有限检索。只找到 X2600 的时钟门控定义，以及 `drivers-x2000` 中的 `x2000_hal_dtrng`/`x2000_ll_dtrng` 空模板；模板有 GPL-2.0-or-later 头部，但没有生产实现。本模块没有复制模板代码，沿用已核对手册的寄存器流程。

进一步查到君正[X26XX官方DTRNG文档](https://gitee.com/ingenic-dev/ingenic-linux-docs/blob/ingenic-master/zh-cn/X26XX/x2670-halley/kernel/DTRNG模块驱动接口.md)及其生产BSP。核查固定提交`147f4697476d24a5ff8522773d68e99709e7c67e`：

- [X2600 DTS第387行](http://git1.ingenic.com.cn:8082/gerrit/plugins/gitiles/kernel/kernel-5.10/+/147f4697476d24a5ff8522773d68e99709e7c67e/module_drivers/dts-stage/x2600/x2600.dtsi#387)将`0x10072000`绑定到`ingenic,dtrng`，因此不是仅凭相似型号类推。
- [生产驱动第141行](http://git1.ingenic.com.cn:8082/gerrit/plugins/gitiles/kernel/kernel-5.10/+/147f4697476d24a5ff8522773d68e99709e7c67e/module_drivers/drivers/char/hw_random/ingenic-rng.c#141)先调用Linux的`get_random_bytes()`，再与硬件字异或后交付。注册hwrng时没有设置芯片专属`quality`。
- [该Linux 5.10的hwrng核心第43行](http://git1.ingenic.com.cn:8082/gerrit/plugins/gitiles/kernel/kernel-5.10/+/147f4697476d24a5ff8522773d68e99709e7c67e/drivers/char/hw_random/core.c#43)默认`default_quality=0`，未据此给硬件输出声明满熵。不能把Linux混合输出的用途说明直接等同于裸机单一DTRNG输出的质量。

这些证据支持寄存器和生产用途，但没有消除本工程的TLS熵量缺口。向厂商补齐两项即可：X2600M具体芯片版本及当前PCLK/divider/inverter配置下，`RANDOMNUM`是否为可直接播种DRBG的满熵后处理输出；若不是，需每32位输出的保守最小熵与推荐调理方式。另需对应的启动丢弃、合法时钟、连续健康检测和故障处理要求。

## 接入方式

网络启动任务在调度器启动后、`tcpip_init()` 前调用 `ai_board_entropy_init()`；初始化只读取并丢弃两个字，验证 ready 流程和连续读取能工作，不进行大量采样。成功后可注册：

```c
ai_lwip_set_random_source(ai_board_entropy_lwip, NULL);
```

MAC 地址使用相同回调取得 6 字节后，仍由调用者设置单播、本地管理地址位。初始化和后续读取只能在任务上下文调用。初始化由一个启动任务独占；正常读取与状态查询通过 mutex 串行化。不要与独立 `ai_board_rng_probe()` 诊断同时运行；若硬件已被启用，初始化返回 `-EBUSY` 并恢复原时钟门控。

初始化和每次有效读取最多使用 10 ms 的总硬件等待预算，mutex 最多等待 50 ms。每次读取允许 1～32 字节，不足一个字的尾部按需使用，其余字节丢弃。集成阶段提供的实板节拍为 divider=1023 时约 0.94 ms/32 位字；32 字节需要 8 个字，约 7.52 ms，保留约 2.48 ms 的预算余量。此前 64 字节上限需要约 15.04 ms，会把正常采样误判为超时，已收紧。当前 lwIP 请求 4 字节、启动 MAC 请求 6 字节，均在范围内。

每个新硬件字都与上一个字比较，包括不同调用之间的边界。一个正常随机源也可能偶然重复；这里的停机规则是保守的卡死检测，既不是熵量估计，也不声称完成某种标准健康测试。

有效长度的请求若失败，会清零整个调用输出，部分采样结果不会被当作成功返回。重复及硬件超时故障锁存到重启，重复调用 init 不会自动清除；mutex 等待失败返回失败，不改动其他任务正在持有的硬件状态。`ai_board_entropy_get_status()` 返回配置、状态、故障码、硬件字计数、读取调用计数与成功交付字节计数，不返回原始随机字。状态中的 configuration 是采样时采用的配置；故障后硬件 `gen_en` 已关闭。

## Mbed TLS 4.1.1 的实际缺口

已固定的发行包中，`tf-psa-crypto/drivers/builtin/src/entropy_poll.c:56` 要求 `estimate_bits >= 8 * len`，否则返回熵源失败。当前 `tls/ai_tls.c` 的适配同样要求回调为完整输出提供 `length * 8` 位熵。

本服务仅获得可持续读取的硬件字，还没有与实际芯片、分频和线路配置对应的质量依据来填写该熵量。因此 `ai_board_entropy_tls()` 保留完整 TLS 回调签名，但始终清零输出和 `entropy_bits`，返回 `-ENOSYS`；不调用硬件，也没有“统计通过即启用 TLS”的开关。加入 SHA 或 DRBG 本身不会提供缺失的输入熵界限。后续应先补齐有效输出质量或保守最小熵界限，再确定是否需要额外收集与调理，随后接入现有 TLS 回调。

## 本次验证

```powershell
python projects/halley7m-ai/network/tests/check_entropy.py --mips
```

2026-10-23 实际结果：MSVC C11 的 11 个独立进程场景全部通过，覆盖正常启动/幂等调用/跨字节读取、实测节拍下 32 字节成功及 33 字节拒绝而不锁存故障、跨调用重复、部分输出失败清除、运行与启动超时、启动重复、硬件已有所有者、mutex 超时、未初始化/分配失败/参数错误，以及 TLS 始终拒绝并报告零熵量。节拍测试按每个 32 位字延迟 940 μs 提供 ready，确认 32 字节在 10 ms 内完整交付。每个故障场景启动独立进程，生产接口没有为测试增设清除故障入口。

XBurst2 GCC 7.2 也已用真实 MMIO、SDK 与 FreeRTOS 头文件编译 `board_entropy.c`，启用 `-Wall -Wextra -Werror`。测试使用的寄存器与锁是 mock；本次没有打开串口、运行实板持续随机服务或执行 TLS 握手。主工程接线和实板验证由以太网集成阶段完成。

随后完成以太网RAM集成：真实DHCP、DNS及SNTP启动期间持续服务状态为READY、错误0，已正常交付网络随机字节。具体固件散列和诊断计数见[主工程验证记录](../VALIDATION.md)。这个实板结果不改变TLS质量边界。

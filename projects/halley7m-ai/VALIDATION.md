# AI 终端验证记录

日期：2026-10-23。此记录逐项区分PC真实云请求、主机C测试、交叉编译、RAM实板与NOR安装。

## 已通过

- DeepSeek官方API：单轮及最终12轮文字场景通过，12轮首文字中位633.8ms、范围332.4～862.7ms。
  第10轮实际背景只包含第2～9轮，第12轮新会话背景为空；详见 [PC探针记录](cloud-probe/VALIDATION.md)。
  这不是板端P95指标；少于每种网络30个样本，且没有设备端音频播放耗时。
- 32项离线Python协议测试通过，覆盖DeepSeek SSE及百炼WSS事件流程。
- MSVC Release真实C/LVGL CTest通过：历史/配置/资源、GUI、Air音频核心。
  Release测试保留断言；GUI输出真实1280×720截图，已修复键盘默认对齐造成越屏的问题。
- DeepSeek设备C请求/SSE协议11组测试通过，加入主工程后4个CTest目标全部通过；
  真实事件结构回放与实际云调用分开记录，尚无板端HTTP传输。
- 音频低通重采样：1kHz增益0.999967，9kHz混叠−74.59dB，10kHz−79.82dB；
  协议拆分、粘包、错误重同步、PTT停止等待和长度边界通过。
- lwIP FreeRTOS端口、IPv4/API/SNTP与板级诊断共44源XBurst2编译通过；双网策略主机测试通过。
- Mbed TLS4.1.1适配：RSA/ECDSA各完成TLS1.2/1.3本机真实握手与传输，错误hostname/CA及过期/未来证书被拒绝；
  未就绪/低熵拒绝、短读写WANT及关闭状态测试通过。MIPS库编译及保留公开入口的可重定位链接通过，
  text162,702/data8/bss1,100 B；不含libc/libgcc增量、生产CA和动态堆，不是最终镜像增量或板端TLS验收。
- 基础MIPS ELF/BIN入口与加载边界校验通过，使用54MiB主核范围；当前镜像适合原NOR应用区，尚未写入。
- 用户指定的G盘FAT32 TF卡：新增 `/ai` 资源及配置，8个文件逐一写入后回读一致。
  字体SHA256 `c540822c0994f9f8433e33b47147ed2492cb1806517cb5ef2333828f8ccbcf4e`；
  词库SHA256 `61f4798bd916b2040830b18da67a798abf63ed046576f6e107006cdbc54a5bb2`。

## RAM 实板基础联调

R1：343,088 B，SHA256 `49477b87c5a368149178af7463ec6526402b508401f3f68bb3e34664c1f9da9b`。
YMODEM后长度和板端CRC32 `273ba62c`一致，跳转后AI_BOOT确认主核运行及RISC-V保持复位。
TF mount/font/dictionary/config全部返回0，key_present=1；GUI/GT911/三键初始化返回0。
初次显示诊断记录140次提交/140次确认，mismatches/underruns为0。
用户随后明确确认显示、nihao候选选择、发送失败保留输入、Start未就绪提示均正常。
这不构成已有整屏频闪已修复的证明。

R1的PHY探测被AHB0 360MHz范围检查提前拒绝，未访问管理接口。
依据X2600手册表4-9确认GMAC属于AHB2，修正为检查实际AHB2 300MHz后重新构建并装载R2。

R2：343,152 B，SHA256 `4af6ce67a168ed552d2047e7d9c4d4fd47363a5a7c14ac7bb2d4e6f8cae8c085`，
板端CRC32 `a60bd6e8`一致；PHY读取返回0，MAC版本`00002037`、MACPHYC0=`40000002`、
PHY地址0、ID=`02430c54`、BMSR=`7849`。BMSR当次未显示link，诊断发生在PHY复位后，
不能据此判断网线状态；没有进行数据帧验收。模式2是当前引导器遗留值，不因此认定手册差异已解决。

两个版本共完成3次256字DTRNG诊断，每次变化掩码`ffffffff`，相邻重复、全零、全一计数均0；
1比特数分别4040、4061、4146（每次8192比特）。有限统计仅说明这次寄存器采样正常，不代替持续熵服务验证。
GUI保持运行，R1/R2诊断前后空载剩余堆均45,671,488 B；不代替完整功能峰值或两小时测试。

原始ELF/BIN/map/manifest与串口日志保存在 `build/ai-foundation-20261023/r1`、`r2`。
公开摘要见 [RAM基础证据](validation/ram-foundation-20261023.json)。未擦写NOR或修改持久启动环境。

## 未通过或尚未执行

- 初次RAM装载遇到COM52暂时消失，未上传或执行AI镜像；随后串口恢复，R1/R2装载均通过。
  首次失败日志 `build/ai-foundation-ramload.log` 保留，后续成功不掩盖此记录。
- P1缺失/损坏资源实板、SW3/SW4实际交互；已有整屏频闪仍是独立未解决问题。
- TLS所需DTRNG输出质量依据、板端TLS完整证书校验；以太网实板进展见下节。
- 百炼真实WSS及文字/语音混合会话；DeepSeek文字成功不替代这些验收。
- Air UART引脚、电平、DMA、10分钟录放音和停止播放尾音。
- SWT6621-SH匹配驱动/固件/NV、双网选择和拔插异常、两小时满负载内存、费用及响应P95。
- NOR安装、完整回读、10次物理断电冷启动。

实际API Key不在日志、报告或提交中；密钥存在性与云端请求成功仅说明当前PC验证环境可用。

## RAM 以太网联调（2026-10-23）

本节为独立以太网功能的证据，不表示P2整个阶段或云端业务通过。新增GMAC轮询DMA、lwIP任务、
DHCP/DNS/SNTP与持续DTRNG服务；所有候选只装载到RAM，未写NOR。

| 候选 | BIN字节 | SHA256 | 板端CRC32 |
|---|---:|---|---|
| Ethernet R1 | 438912 | `e9173ab1301c469cefed3e7d441aad8791f425b1b1c17212b95068694c885a76` | `b6a79471` |
| Ethernet R2 | 440720 | `3a579bdde9fc20fe39e3af1b9a41cc6637e6bb283d6a42a285dd8f5e29ce9a8a` | `4864f7a6` |
| Ethernet R3 | 469792 | `ed5cfb837c9739222955b431bd51b39787962e3b31074425488ababa61b9e38a` | `1a7b3369` |
| Ethernet R4 | 470304 | `79f8c16c0b941b8c87fea4342654cf1526928d1b9bd4a98b744c30a2136d77b2` | `63f12419` |
| Ethernet R5 | 472016 | `5d6c6b95cea694a3b246c4b64efd4f3c3c872ed7428713528c09bc718cc4f4b5` | `72e6ab5f` |

五版YMODEM长度和CRC验证均通过。R1/R2均协商为100Mbps全双工，PHY地址0/ID`02430c54`，
模式`40000002`、HW_FEATURE=`490d0935`。DHCP分别获得`192.168.1.101`和`.102`，
网关/DNS均`192.168.1.1`；`api.deepseek.com`解析为`61.170.66.121`，UTC就绪。
当前本地管理MAC每次启动随机生成，所以两版地址不同，不解释为DHCP漂移故障。

R2修复应用停止后lwIP内部NTP DNS晚到问题：应用自行带查询token/网络代次解析，再给SNTP固定IP；
地址就绪以DHCP实际状态为准。用户实际拔插网线一次，串口完整记录`link=1→0→1`，
断线时`address_ready=0,dns_ready=0`，重连先保持未就绪，租约确认后同一`.102`恢复；
代次从4→5→6→7，NTP/DNS服务重新启动，未见DMA错误或输入/输出丢弃。

R2最终采样：RX24/TX21，错误及丢弃均0；DTRNG状态READY、错误0、26个硬件字、23次读取、
交付94字节。GUI返回0，步骤75837，剩余堆45,162,688 B，网络任务剩余栈最小3738 words。
显示诊断925次提交/925次确认、underruns=0、pending=0、mismatches=4。
按`player_display.c`及既有`player_display_test`，该计数表示EOD核验时描述符/源地址/SITE暂未匹配，
保留pending后继续核验，并不等于4次丢帧；本次最终全部确认。没有失败当刻采样来确定具体原因，
不据此认定新增可见回归，也不自动归因于既有频闪，仍保留实际计数。

电脑WLAN与板端LAN的Ping未通过：R1的12个1472B请求没有Echo Reply；R2的6个1472B请求全部超时。
R2在这次Ping前后RX11/TX12及ARP RX5/target_us1/reply_submit1均未增加，电脑ARP为Unreachable；
普通32B Ping也未收到Echo。之后R4读取网关ARP，发现板端与电脑的网关MAC不同；
用户进一步确认两者实际连接不同路由器，恰好都使用192.168.1.x地址段。因此这些电脑到板端的Ping
不作为驱动失败依据；同一局域网互访测试等待接线条件具备。用户当前要求保留现有接线。

本次主机验证：4个C/LVGL CTest目标、双网策略测试、44源MIPS编译全部通过；
持续随机服务11个mock场景及真实头文件MIPS编译通过。没有新增云API调用。
TLS仍待厂商确认裸DTRNG输出性质与推荐使用条件；官方Linux驱动混入了Linux已有随机源，
不能直接作为裸DTRNG满熵依据，固定来源见[随机服务说明](network/board_entropy.md)。

完整ELF/BIN/map/manifest及串口、Ping日志保存在`build/ai-ethernet-20261023/r1`、`r2`。

R3加入`net ping`网关诊断并启用RAW socket，RAM长度/CRC验证通过。DHCP获得`.103`，DNS/UTC就绪；
实际执行一组1472B payload网关Ping，`sent=4 received=0 ignored=0 result=-116`，未通过。
DMA TX由12增至16、没有发送错误；RX16→17同时ARP10→11，仅见背景ARP，没有ICMP回复进入接收计数。
电脑对其自身网关的1472B Ping为2/2通过，但后续确认不是开发板所连接的网关，不能作为同一目标对照。
尚未确定板端大帧失败原因，不能把发送完成当作大帧往返通过。
R3日志和固件保存在同级`r3`目录，后续小包对照结果另行记录。

R4固定32B小包对照：4/4 Echo通过，RTT为2/1/1/1ms；随后的1472B仍为0/4，均超时。
RX由11增至15、TX12增至20，新增4个RX对应小包回复；描述符错误/输入输出丢弃仍0。
DHCP获得`.104`，域名解析/UTC就绪，GUI返回0，剩余堆45,134,016 B。固件及完整对照日志在`r4`目录。
当前RX软件错误计数只覆盖交付DMA的帧；RSF开启且FEF关闭时，错误帧可能已在MAC FIFO被丢弃，
不能用DMA错误为零排除线上坏帧。后续读取硬件MMC统计单独验证。

R5为本次冻结固件，增加只读MMC采样，PHY/收发策略保持不变。实际`available=1`来自HW_FEATURE能力位，
控制寄存器0（没有读清零、没有冻结），但所有TX/RX MMC计数在真实收发前后都保持0。
这些硬件计数目前没有成为有效观测，**不能将零CRC/零overflow解释为排除了线上错误**。
需要进一步核对这块芯片的MMC实现或使用外部抓包；没有猜测修改PHY模式和长度门限。

R5的32B Ping再次4/4通过，RTT2/1/1/1ms；1472B仍0/4。比较期间DMA RX20→25、TX12→17→21，
小包期间另有1次ARP请求及回复，大包期间没有新增RX。描述符错误和丢弃计数均0。
DHCP地址`.105`、DNS/UTC就绪，GUI返回0，剩余堆45,129,920 B，网络任务最小剩余栈3702 words。
本次没有持续满载、两小时、板端TLS、NOR写入或物理冷启动验收。原始日志在`r5`目录，
公开结构化摘要见[以太网RAM证据](validation/ethernet-20261023.json)。

# 播放器 NOR 安装与启动

当前NOR为“GT911触摸版（2026-10-21，候选R8）”，新增屏幕板载触摸和深色播放器布局，
沿用原R9的控制条隐藏/唤出、时长显示，以及已有音视频兼容性。用户已确认RAM阶段
触摸方向、点选、菜单及布局正常，并明确要求将当前候选写入Flash。默认音量7%，
启动音频关闭；频闪仍未解决，完整播放/UI序列因TF读取错误未通过。

## 当前安装：GT911触摸版（2026-10-21，候选R8）

此“候选R8”为本轮触摸适配的版本编号，与下文2026-10-20的历史兼容性R8区分。

| 项目 | 值 |
| --- | --- |
| 冻结源 | `build/player-touch-ui-20261021/r8/Release` |
| 原RAM合包 | 1974368B，CRC32 `3ea4ae8b` |
| 原RAM SHA256 | `5c045992257be35a9615b50e997bbdff3806d850bee08116e7284b67a7296d2c` |
| NOR安装包 | 2326528B / `0x238000`，CRC32 `286de5e5` |
| NOR SHA256 | `81ef2455f076a71d14d0320433931df6b4ce06a6699fdf63cdc3c7af70a5c705` |
| 替换的R9安装包 | 2326528B，CRC32 `3d7e43a0` |
| 应用写入/擦除范围 | `[0x40000,0x278000)`，沿用旧范围 |
| 环境区 | `[0x3f000,0x40000)`，保持原启动命令和1秒等待 |
| 保护区前后CRC | SPL/U-Boot代码 `3a3c3918`；当前rootfs `8618c73f` |

冻结合包原有字节不变，追加352160B FF；原文件末端`0x807e2060`，BSS为
`[0x807e2080,0x809141b0)`，填充末端`0x80838000`在启动清零范围内。
先核对真实ELF/BIN、旧R9安装身份、原始双份备份及板端旧完整应用区和有效环境，
再擦写应用范围。安装JSON为`programmed_and_verified`：2326528字节完整回读比较与
CRC通过，持久环境CRC有效，SPL/U-Boot代码及rootfs校验保持不变。
启动命令仍为`sfcnor read 0x40000 0x238000 0x80600000;go 0x80600000`。

首次软件复位后，日志确认由NOR自动读取`0x238000`字节并跳转，出现`PLAYER_BOOT`、
`AMP_READY`，显示/GUI/三键/GT911初始化结果均0，默认音量7%、启动音频关闭。
尚未进行本版物理断电冷启动，安装JSON保留`cold_boot_verified=false`；不迁移旧版
冷启动或30分钟结果。频闪与两次TF读取失败保持为已知问题，用户授权部署不等于
这些问题已修复，也不将用户对硬件原因的判断写成已证实结论。

独立内嵌三秒片冒烟未达到零掉帧要求：81解码/78呈现时检测到drop/late 1/1、
max_late 33334us，脚本立即STOP，未播放到EOF。清理后82解码/80呈现、
129600/144000有效音频样本，pinned0、会话error0、音频inactive且音频错误全0。
不将此次安装成功写成完整短片或尾段验收通过，详细边界见[验收记录](VALIDATION.md)。

随后另作一次软件复位，`nor-final-boot.json`确认正常NOR自动装载和初始化，未启动
播放；早期GUI采样为rendered1/presented0。后续静止查询`nor-final-settled.json`为
passed：IDLE、会话音量7%、pinned0/error0、音频inactive、媒体库GUI1/1且error0，
GT911 ready、6463次poll且error0。最终保留此静止媒体库状态；本次没有物理断电验证。

当前派生包和manifest在`build/player-touch-ui-20261021/nor-install/`，
安装JSON/log为同目录`flash-install.json/.log`。下一次更新的previous应使用这次
实际写入的派生包和安装JSON，旧R9安装改为历史记录。

| 当前安装证据（`build/player-touch-ui-20261021/nor-install`） | SHA256 |
| --- | --- |
| `flash-install.json` | `b7e02cc851883a35f81f056a068292777c72ed2bb771d128f75e44f3d5d8aa86` |
| `flash-install.log` | `361a635ce794d9a86fd016986a1e62eb18ee9f76b2970ae7c0c7c7ac801d4a5a` |
| `nor-smoke.json`（未通过） | `ce5e3a7757a93e1faaa7393c87a13d56a1bed85884dc36eab71bc741d4eb5946` |
| `nor-smoke.log` | `8b35aaba09a80b1b936d39a3c7d13cff48313f2dd2157ccf499425f3192f6b99` |
| `nor-final-boot.json` | `54f2fad7cde7dc31c8123dff878bab7d1119a73f7c6260ba686187ce24215a5e` |
| `nor-final-settled.json` | `7ca62e29f6ef5498087d16eef0232fdb64db215e96af949661fb335392ccb8f4` |

## R9播放UI与时长修复（历史记录）

| 项目 | 值 |
| --- | --- |
| 对应功能源码 | `7e7d61a` |
| 原RAM合包 | 1951392B，CRC32 `d4131fe7` |
| 原RAM SHA256 | `17aae0405f08bc2864585b61de0699220fd83f33c7016c6b0ff8471a115c869f` |
| NOR安装包 | 2326528B / `0x238000`，CRC32 `3d7e43a0` |
| NOR SHA256 | `52e79b4d8dddfd97cdc2073105efeeee5da03d92ab788e4cc9b88623dd88e83c` |
| 替换的R8安装包 | 2326528B，CRC32 `b8c0bfad` |
| 应用写入/擦除范围 | `[0x40000,0x278000)`，与上次相同 |
| 环境区 | `[0x3f000,0x40000)`，保持原启动命令和1秒等待 |
| 保护区前后CRC | SPL/U-Boot代码 `3a3c3918`；当前rootfs `8618c73f` |

先核验冻结ELF/BIN、BSS边界、原始双份备份、当前R8完整应用区及有效环境，再擦写
应用范围，全部2326528字节回读比较与CRC通过。原代码包不变，追加375136B FF；
BSS为`[0x807dc6c0,0x8090e6c0)`，填充末端`0x80838000`在启动清零范围内。
持久命令仍为`sfcnor read 0x40000 0x238000 0x80600000;go 0x80600000`。

安装后独立核验从NOR自动启动、媒体库、双核、默认7%及音频关闭。吉他MP4开头
已显示115750816us、音频实际总数仍未知，空闲隐藏/暂停常显通过，音频错误均0。
本轮短播134帧解码/130呈现、drop/late4/4、max_late61000us，STOP后pinned0、
音频inactive；随后再次软件复位直接从NOR进入媒体库。未进行物理断电重上电，
`cold_boot_verified=false`，不把此前R8的冷启动确认迁移到R9。

该次派生包为`build/player-ui-fixes-20261020/nor/halley7m_player_nor.bin`，
manifest在同目录；安装JSON/log为`build/player-ui-fixes-20261020/flash-install.json/.log`，
独立启动/播放记录为同目录`nor-smoke-board.log/.json`。散列及RAM验收见
[验收记录](VALIDATION.md)。此次更新已被上述GT911触摸版替换，保留原冻结文件和证据。

## R8安装（历史记录）

| 项目 | 值 |
| --- | --- |
| 对应功能源码 | `c0e5033` |
| 原RAM合包 | 1949856B，CRC32 `be8b7739` |
| 原RAM SHA256 | `bad023ccbf951a25fec04e30ecbb08f356fa0a973ae867b6b56c845d5e6bba80` |
| NOR安装包 | 2326528B / `0x238000`，CRC32 `b8c0bfad` |
| NOR SHA256 | `805c15d899246f81aa59a29ea623fd37ce1353d2e85db3f3c71aec9cf1090861` |
| 替换的R4安装包 | 2326528B，CRC32 `a046b4d8` |
| 应用写入/擦除范围 | `[0x40000,0x278000)`，与上次相同 |
| 环境区 | `[0x3f000,0x40000)`，保持原启动命令和1秒等待 |
| 保护区前后CRC | SPL/U-Boot代码 `3a3c3918`；当前rootfs `8618c73f` |

先核验双份原始16MiB备份、本地上次安装身份及真实ELF/BIN，再由播放器确认STOP/
音频关闭并软件复位进入U-Boot。新包上传后重新匹配整段6MiB旧应用区和持久环境，
才擦写上述应用范围；写入CRC和全部2326528字节比较均通过，环境及保护区复核通过。
保持启动命令`sfcnor read 0x40000 0x238000 0x80600000;go 0x80600000`。
随后reset确实由NOR启动，观察到PLAYER_BOOT、AMP_READY及DISPLAY/GUI/BUTTONS_INIT=0；
未把软件复位写成物理断电冷启动通过，安装JSON仍为`cold_boot_verified=false`。

后续2026-10-20单独补做一次断电约5秒再上电，用户确认进入媒体库且没有频闪；
串口记录NOR自动读取/跳转、双核握手和显示/GUI初始化成功，音频未启动、默认音量7%。
冷启动原始记录为`build/player-flicker-20261020/flash-cold-boot-01-board.log/.json`，
因USB重连缺少完整SPL/PLL段。安装JSON保留当时状态，不回写后续验收；本轮未写Flash、
未改背光/扫描参数，不能将未复现记作频闪修复。对照及散列见[验收记录](VALIDATION.md)。

新NOR包仅追加376672B的FF；原文件末端为`0x807dc0a0`，BSS为
`[0x807dc0c0,0x8090e080)`，填充末端`0x80838000`在启动入口清零范围内。
本地产物为`build/player-nor-compat-r8-20261020/{halley7m_player_nor.bin,manifest.json}`，
安装证据为`build/flash-player-nor-compat-r8-20261020.log/.json`。

复位后的独立查询为AMP active/owner=riscv、会话IDLE、媒体库页、音频关闭，默认音量7%。
从NOR运行时完整播放TF原始`test.mp4`：994帧解码、989呈现、drop/late5/5、
max_late67000us；1592520有效音频样本完整，DMA错误/超时、队列/FIFO欠载均0，
`weight_approx=5`。STOP后IDLE、音频inactive、pinned0、空闲堆34650432B，
宽幅GUI仍持有CUSTOM表面。原始记录为`build/player-nor-compat-r8-boot-play-board-20261020.log`；
不将完整播放描述成零掉帧，也不重跑或迁移此前30分钟结果。

| 当前安装证据 | SHA256 |
| --- | --- |
| 安装/自动启动日志 | `e0e876bdf08b2725abfd2fd6c28b0ae7b47da5a2dbdb73476159cd9dcb9f8e07` |
| 安装JSON | `936dcd65a19c23b76ff620a7bee2a922e36a303d0c559b648618e51cea7669fd` |
| 独立启动/播放日志 | `d6271fcdf4405d977464e75c86444080ad1cd8f8ef8fd4cd9be5ac1183aab2ce` |

## 首次安装R4（历史记录）

2026-10-20首次安装时，按用户要求将此前已通过 RAM 板测的R4播放器安装到本板 NOR。
软件复位后已由原 U-Boot 自动读取播放器，双 FreeRTOS、LVGL 和三键初始化通过；
三秒 MP4 冒烟为90/90帧、drop/late0、144000有效样本完整且音频错误0。
实际断电上电由用户随后拷卡测试，本记录不将软件复位写成冷启动通过。

### 首次安装身份与范围

| 项目 | 值 |
| --- | --- |
| 原 RAM 合包 | 1920032B，CRC32 `1e71d376` |
| 原 RAM SHA256 | `2ff6503f4650481d6339c64c21ecbe39e9670f93cd1f1d0977ba9a8abe1553c6` |
| NOR 安装包 | 2326528B / `0x238000`，CRC32 `a046b4d8` |
| NOR SHA256 | `2a625f88915fcb01bfdc52656bbdedc9befbea5ffddf59eca65875c22c297a58` |
| 先前 ReGBA | 2306352B，CRC32 `178853ea` |
| 应用写入/擦除范围 | `[0x40000,0x278000)` |
| 环境区 | `[0x3f000,0x40000)`，保存 bootcmd 和 bootdelay |
| 默认音量 | 7%，启动不播放音频 |

当前持久启动命令为：

```text
sfcnor read 0x40000 0x238000 0x80600000;go 0x80600000
```

保留原 SPL/U-Boot 代码和本次操作前 rootfs，前后 CRC 分别为 `3a3c3918`、
`8618c73f`。启动等待仍为1秒。此安装让播放器成为上电默认应用，未增加启动选择菜单。

NOR包由原RAM合包追加406496B的FF生成，原代码、字体、内嵌小核和片源字节均不变。
填充沿用旧应用的32KiB对齐擦除范围，避免较小的新程序留下旧尾；派生包有独立
`player-nor-padded` manifest，不将其散列冒充原RAM合包。
ELF核验原文件末端为`0x807d4c20`，其后32B为对齐空隙；BSS范围为
`0x807d4c40..0x80902380`，填充末端`0x80838000`在其中。
`mips/start.S`在进入C环境前清零BSS，软件复位实测也已验证该包能正常启动。

## 工具入口

[flash-player-nor.py](../../scripts/flash-player-nor.py) 默认仅核对本地文件、生成NOR包并打印计划。
它核验播放器嵌套manifest、真实ELF/BIN、7%默认音量、上次安装记录和原始双份16MiB备份，
实际擦写复用既有`flash-regba-nor.py`。擦除前需匹配旧完整6MiB应用分区和持久环境；
写入后检查FF尾、全部程序字节、环境CRC/bootcmd，以及启动代码和rootfs保护范围。

首次安装由已验证的U-Boot RAM上传使用`--reuse-ram`执行，擦除前再次校验整包RAM CRC。
该选项不能与复位选项一起使用。`--reboot-player`可用于已运行播放器的后续更新，
先确认STOP已应用且音频关闭，再软件复位；`--wait-boot`用于捕获物理复位。

```powershell
python scripts/flash-player-nor.py --self-test
```

后续版本的本地准备示例（使用当前GT911触摸版安装作为previous，换用新的output目录）：

```powershell
python scripts/flash-player-nor.py `
  --file build/player-riscv-Release/mips/halley7m_player.bin `
  --manifest build/player-riscv-Release/manifest.json `
  --previous-bin build/player-touch-ui-20261021/nor-install/halley7m_player_nor.bin `
  --previous-install build/player-touch-ui-20261021/nor-install/flash-install.json `
  --output build/player-nor-next
```

检查计划后，执行写入需显式增加`--execute`，按当前板端状态选择启动选项并指定新日志。
保存此次NOR派生BIN、manifest和安装JSON；下一次previous必须使用实际烧入的派生包。
它们与原始备份、完整日志均保留本地，不提交二进制。

## 首次安装实板证据

- `build/player-nor-20261020-package-ramload.log`：2326528B长度和RAM CRC通过，no-go。
- `build/flash-player-nor-20261020.log/.json`：旧分区和环境匹配、擦写回读、2326528B
  逐字节相同、完整环境CRC与保护区验证；reset后自动NOR加载及`PLAYER_BOOT/AMP_READY`。
- `build/player-nor-20261020-boot-smoke-board.log`：初始IDLE、媒体库页、音频关闭；
  MP4到ENDED时90/90、drop/late0、144000样本且音频全错0；STOP后IDLE、kind0、
  pinned0、音频CLOSED/inactive、GUI无错。宽幅GUI仍持有旋转表面，属于显示常驻量。

| 本地证据 | SHA256 |
| --- | --- |
| RAM上传日志 | `79564a7a0ed82feecd407664cf1c1435bf7f8134c548e92f7eee3995b781763b` |
| NOR安装日志 | `e1297dd4e8ef2491dec4ac54abe57f1400811186461a5f119f6890518cd0658f` |
| NOR安装JSON | `7998c2fdada956d0425c2fef779e03ef8fff80f59fd0e0c3cf210f9a4026c2ba` |
| 启动/播放冒烟日志 | `a58d4722f7ba879f0d6edbdf1d6c377a7171a8e4b5d3a442b975835280c2cd1a` |

## 断电拷卡后使用

停止播放后正常断电，取TF卡，将PC生成的
`build/player-converted/guitar-player-1280x720.mp4`复制到卡的`multimedia/video`。
插回开发板后直接上电，由NOR启动新版播放器，再从媒体库选择。
该副本为1280×720，原比例1280×656加上下黑边；以46ms实际黑帧替代起始空编辑，
原画面PTS和AAC载荷/PTS均保持。PC全片解码和生产解析通过，真实TF已完成R9的
开头时长、控制条隐藏/唤出和暂停回归；本轮片段测试不作为全片零掉帧验收。

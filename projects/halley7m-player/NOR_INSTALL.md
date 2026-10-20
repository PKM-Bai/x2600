# 播放器 NOR 安装与启动

当前NOR为2026-10-20更新的兼容性R8版本，包含真实时间戳、Main/High、CABAC、
B帧、常见尺寸/SAR及601/709色彩处理。更新源为已完成RAM验收的冻结产物，
没有重新构建或修改其代码字节；默认音量7%，启动音频关闭。

## 当前安装：兼容性R8

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

后续版本的本地准备示例（使用当前R8安装作为previous，换用新的output目录）：

```powershell
python scripts/flash-player-nor.py `
  --file build/player-riscv-Release/mips/halley7m_player.bin `
  --manifest build/player-riscv-Release/manifest.json `
  --previous-bin build/player-nor-compat-r8-20261020/halley7m_player_nor.bin `
  --previous-install build/flash-player-nor-compat-r8-20261020.json `
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
原画面PTS和AAC载荷/PTS均保持。PC全片解码和生产解析通过，实际TF播放待拷卡后测试。

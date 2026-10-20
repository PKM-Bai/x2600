# 播放器 NOR 安装与启动

2026-10-20，按用户要求将当前已通过 RAM 板测的播放器安装到本板 NOR。
软件复位后已由原 U-Boot 自动读取播放器，双 FreeRTOS、LVGL 和三键初始化通过；
三秒 MP4 冒烟为90/90帧、drop/late0、144000有效样本完整且音频错误0。
实际断电上电由用户随后拷卡测试，本记录不将软件复位写成冷启动通过。

## 本次安装身份与范围

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

本次由已验证的U-Boot RAM上传使用`--reuse-ram`执行，擦除前再次校验整包RAM CRC。
该选项不能与复位选项一起使用。`--reboot-player`可用于已运行播放器的后续更新，
先确认STOP已应用且音频关闭，再软件复位；`--wait-boot`用于捕获物理复位。

```powershell
python scripts/flash-player-nor.py --self-test
```

后续版本的本地准备示例（使用这次安装作为previous，换用新的output目录）：

```powershell
python scripts/flash-player-nor.py `
  --file build/player-riscv-Release/mips/halley7m_player.bin `
  --manifest build/player-riscv-Release/manifest.json `
  --previous-bin build/player-nor-install-20261020/halley7m_player_nor.bin `
  --previous-install build/flash-player-nor-20261020.json `
  --output build/player-nor-next
```

检查计划后，执行写入需显式增加`--execute`，按当前板端状态选择启动选项并指定新日志。
保存此次NOR派生BIN、manifest和安装JSON；下一次previous必须使用实际烧入的派生包。
它们与原始备份、完整日志均保留本地，不提交二进制。

## 实板证据

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

停止播放后正常断电，取TF卡，将PC生成的`build/player-test-mp4/test-player.mp4`
复制到卡的`multimedia/video`。插回开发板后直接上电，由NOR启动播放器，再从媒体库选择。
该副本已通过PC参数及完整解析/解码检查，实际TF播放由本次拷卡后的测试确认。

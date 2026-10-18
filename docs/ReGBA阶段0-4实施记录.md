# X2600M ReGBA 阶段 0～4 实施记录

> 本文保留各阶段当时的实现与验收范围；当前工程状态见[项目 README](../projects/halley7m-regba/README.md)。`build/` 原始日志、镜像与备份，以及 `Halley7/` 原始硬件资料和本机工具包均在开发机保留，不随源码提交；精选摘要在 `docs/validation/`，其中保留的本地路径用于追溯。

本文件保留阶段 0～4 无屏移植时的测试与版本记录。后续阶段 5 已确认原 Linux 实际绑定 ZC50289HSHD02，并在 FreeRTOS 上完成竖屏彩条、真实游戏画面、320 秒显示/静音音频/TF 联合短测，以及修订后的两轮显示开关与 60 秒游戏回归；详见[阶段 5 显示实施记录](ReGBA阶段5显示实施记录.md)。下文无屏阶段的“画面待验”及屏幕准备事项属于当时状态，原有 DDR 全容量、冷启动重复性、实体输入、可闻音质和存档往返等待验收项目继续保留。

更新日期：2026-10-16。已完成完整 NOR 双遍只读备份，并通过原有 U-Boot/YMODEM 在 RAM 启动 ReGBA。RTOS 上下文、46111232 字节应用堆 DDR、JIT、内置核心清缓存重译、TF 重启读回、32MiB ROM 电脑端 CRC 对比及内置音频首轮检查通过。第五版游戏欠载修订后，第六版实际通过 32MiB ROM、非零 PCM、静音 DMA 和并行 TF 读写的 30 分钟压力：主机计时 1800.094 秒、板端 1800.060 秒，运行中队列/FIFO 欠载、错误和超时均为 0；跨过两次 OST 低 32 位计数回绕，堆和栈余量稳定。当前核心与音频均已停止，保持静音，未烧录 ReGBA。DDR 全容量与边界、多次冷启动、游戏兼容性/画面/实体输入、可闻声音质量及存档往返仍未验收，阶段 0～4 尚未全部完成。

用户反馈原 60 秒、1kHz 测试音刺耳后，已将 `tone` 改为 2 秒、500Hz、约 -30dBFS，首尾各 50ms 渐变，并让 `stop` / `audio off` 可取消测试音。该修订已随第五版固件加载到 RAM，本轮未执行 `tone`，短音及取消行为尚未进行声音验证。新增 `audio silent`：保持真实 PCM、采样率转换及 DMA 路径运行，同时强制 codec DAC 静音、PE17 功放关闭，用于后续无声压力测试；不能据此认定扬声器声音质量通过。后续默认静音检查，确需听音时先告知，仅短时一次。

## 本轮交付

独立工程位于 `E:/GitProject/X2600/projects/halley7m-regba`。原 Hello、SDK 和 U-Boot 工程保持原样。构建入口为 `scripts/build-regba.ps1`，详细接线、命令与验收标准见工程 README。

| 阶段 | 软件进展 | 实板验收状态 |
|---|---|---|
| 0 | Hello 基线复编；独立 ELF/BIN/MAP；产物边界验证；NOR 双遍只读备份脚本和 fake transport 自测；应用堆 DDR 检查 | NOR 两遍各 16MiB 备份 verified；YMODEM 长度/CRC 校验及 ReGBA RAM 首启通过；46111232 字节应用堆六模式及 64 项 walking 检查通过；DDR 全容量/边界及连续冷启动待验收 |
| 1 | FreeRTOS 主核 port、1ms tick、64 位 OST 时间、newlib 单堆、完整 GPR/HI/LO/FPU 上下文、异步 RTOS 自测 | FCSR 硬件读回期望修正后，自测 32/32 轮、抢占 102/101 次、OOM 后继续调度通过；30 分钟混合压力首轮通过，两次 OST 低 32 位计数回绕后继续单调，板端与主机计时一致 |
| 2 | SSI0/PB0 手动 CS、SD SPI、FatFs 专属文件任务、stdio RPC、CRC 和 128KiB 文件测试 | 真实 J9 TF/FAT32 挂载、32MiB 读取、128KiB 写入/同步/读回及重启 CRC 复核通过；32MiB ROM 与电脑端字节数及 CRC32 一致；20 次冷启动未验收 |
| 3 | 固定 ReGBA 核心及 MIPS JIT；全量 ROM 最大 32MiB；无屏 RGB555/CRC；自制功能测试 ROM；串口输入和帧边界停止 | 清空 JIT 缓存前后各 120 帧 CRC 一致；32 分支链 ISA 模拟器深度 8 回归通过；第六版 32MiB ROM 混合压力 30 分钟及停止首轮通过，107650 帧无栈越界；游戏兼容性、画面及实体输入未验收 |
| 4 | 内置 codec、48kHz 单声道 DMA、双声道输入与流式转换、音频统计、可取消短测试音及静音 DMA 检查模式 | 第四版实际测试音及播放并行写卡首轮通过；第六版游戏 PCM/静音 DMA 与 TF 并行压力 30 分钟通过，队列/FIFO 欠载、错误和超时均 0，结束停止正常；游戏可闻声音质量与短音取消声音验证仍待验收 |

## 已完成的软件检查

- 官方 XBurst2 GCC 7.2.0、CMake、Ninja：Release 和 Debug 均可编译链接。最终数据以各构建目录的 `manifest.json` 为准。
- ELF32、小端、o32、MIPS32r2，入口 `0x80600000`；没有未解析符号。链接断言限制 NOR 镜像 ≤6MiB，并要求静态布局之后至少留下 36MiB 堆。
- 关闭回放和即时存档的调用路径；修正上游重复定义，使用 `-fno-common` 和 section GC，确认最终 ELF 不含 `SAVESTATE_REWIND_MEM`、`savestate_write_buffer`。
- 修正 JIT stub 的 o32 参数区和栈对齐；在 C 调用路径切换到应用 GP，返回后恢复客体 GP；缓存发布使用严格寄存器约束的汇编，避免破坏客体 CPU 状态。
- 原生存储测试通过 CRC7/CRC16、CSD、CP437、FAT32 RAM 卷和 MBR 分区、长文件名、128KiB 写入/同步/重挂载逐字节读回、EOF/seek、rename/unlink。
- 音频提交函数从生产源码抽取，140 组采样率/输入分块/队列背压组合逐样本比对独立计算结果，全部通过。
- NOR 备份脚本默认不打开 USB；fake transport 测试通过两遍各 16MiB 落盘、CRC/SHA256/长度比对及不一致拒绝。2026-10-16 实板双遍备份亦通过，证据见下节。
- WSL 中的官方 Linux MIPS 指令模拟器完成自测 ROM 的 120 帧：六个 ARM/Thumb/SWI/RAM-code/SMC 检查值正确，完整 240×160 画面为预期 RGB555 `0x3DEF`，VBlank IRQ 计数 118，退出码 0。新增 `--depth-regression` 在诊断 ROM 前追加 32 个 ARM 分支块，实际达到翻译深度峰值 8 并回到深度 0，仍通过全部 120 帧检查；日志为 `build/regba-depth-sim/simulator.log`，执行耗时 24.092 秒、退出码 0。该验证不运行 FreeRTOS，也不模拟 X2600 的真实缓存和外设。

复现原生测试：

```powershell
.\scripts\test-regba-host.ps1
.\.venv\Scripts\python.exe scripts\backup-nor.py --self-test
wsl -d Ubuntu-22.04 -- bash -c 'cd /mnt/e/GitProject/X2600 && python3 scripts/test-regba-sim.py'
wsl -d Ubuntu-22.04 --exec python3 /mnt/e/GitProject/X2600/scripts/test-regba-sim.py --depth-regression --output /mnt/e/GitProject/X2600/build/regba-depth-sim
```

## 2026-10-16 实板调试记录

USB Boot 设备枚举为 `A108:EAEF`，CPU 识别为 `X2600`。实际执行 `scripts/backup-nor.py --execute`，使用 CPU 800MHz / DDR2 400MHz 的官方 RAM helper 读取 Puya PY25Q128H，JEDEC 为 `0x852018`。两遍均读取完整 `16777216` 字节，64KiB 分块的设备传输 CRC 全部通过；落盘长度及 SHA256 相同，`metadata.json` 状态为 `verified`、`forbidden_requests_sent` 为 `false`。备份过程未发送 NOR 初始化/擦写请求，未烧录 ReGBA。

备份目录：`build/nor-backups/20261016T024011Z/`。

| 文件 | 字节数 / 状态 |
|---|---|
| `nor-original.bin` | 16777216 |
| `nor-pass2.bin` | 16777216 |
| `metadata.json` | `verified`，记录输入哈希、JEDEC、读取范围和请求集合 |

两份镜像共同的 SHA256：

```text
5efd956157cf88fc8a55dd3695f75b216e184a9fa15bee00149680c740077926
```

随后用户恢复正常 NOR 启动并复位，通过 COM52 打断自动启动，停在 `x2600_halley7#`。日志 `build/normal-boot-20261016.log` 确认原有 U-Boot 为 `2013.07-00006-g96302ff087`（构建时间 Mar 25 2026），识别 X2600M DDR2、64MiB DRAM 和 PY25Q128H。只读寄存器探测保存在 `build/uboot-probe-20261016.log`：`CPAPCR=0x0300490d`、`CPMPCR=0x04b0490d`、`CPCCR=0x9a0b5410`，与正常 CPU 1.152GHz、MPLL 1.8GHz、DDR 600MHz 配置一致。U-Boot 的 64MiB 识别不替代 DDR 容量及边界压力测试。

原有启动日志还包含 `ERROR EPC 83fdf2e4` 和 `bad CRC, using default environment`，当前仅记录该现象，未修复环境区或写入 NOR。`help loady` 确认可通过 YMODEM 载入 RAM。

### RAM 首启与首轮外设/核心测试

日志 `build/ramload-20261016-105059-ef8199.log` 记录了 YMODEM 传输、板端 CRC 校验和 `go 80600000`。首轮 BIN 为 `705040` 字节，SHA256 为 `dfd7e814cf1e469d0350fbd0c2b4dee7755210cb8bcda781ee194f6fd30a747b`；U-Boot 报告长度相同，RAM 中 CRC32 为 `dc7b0915`，校验后启动并打印 ReGBA 命令列表。该过程仅载入 RAM，未烧录 NOR。

以下结果来自 `build/regba-bringup-20261016.log` 的首轮固件测试；日志会继续追加，后续修订版结果须另行记录。

| 检查 | 实际结果与范围 |
|---|---|
| `status` | `tasks=5`、初始 `heap_free=48429744` 字节；两次状态查询中 `time_us` 和 tick 同步前进，未完成计时精度或回绕验收 |
| `rtos-test` | `FAIL: context=5/5 rounds=0/0 preemptions=0/0`，FCSR 比较待定位；不能判定上下文保存恢复通过 |
| `jit-test` | `PASS: same-address rewrite 37 -> 73 across ticks`，同地址重写代码及跨 tick 执行首轮通过 |
| `core-test` | 120 帧，`elapsed_us=1998574`，`RGB555_CRC32=19b3e686`；mailbox 为 `42,55,66,77,6,0x600D`，VBlank IRQ 计数 118；ARM/Thumb/SWI/IRQ/RAM-code/SMC/完整帧像素检查通过 |
| `mount` / `ls` | FAT32 挂载成功：`sectors=62333952`、每簇 64 扇区，能列出现有目录和 ROM 文件 |
| `prepare` / `rwtest` | 创建资源目录并新建 `/GBA/TEST/SDTEST00.BIN`，`131072` 字节、CRC32 `29df376d`；写入、同步、关闭及读回校验通过，文件保留供重启复核 |
| `crc /gba_game/test1.gba` | 读取 `33554432` 字节，CRC32 `fc7b376f`；首轮当时尚未与电脑端比较；后续已匹配，见电脑端 ROM 校验记录 |
| `tone` | 立即返回 `result=-1`；随后音频状态仍为 `running=0`、`DMA_done=0`，未进入 DMA 播放，不能判定测试音或音频通过 |
| `load /gba_game/test1.gba` / `run 120` | 32MiB ROM 载入成功，剩余堆 `14870744` 字节；运行后报 `Stack overflow: ReGBA`、`PANIC: task stack overflow`，不能判定游戏运行通过 |

上述为首轮失败记录，后续修订与复核结果如下；不能将首轮问题继续视为当前未定位问题，也不能把短时通过视为整阶段完成。

### 修订版实板复核

第二版载入日志为 `build/ramload-20261016-111724-8c6595.log`，BIN 707824 字节、SHA256 `eaf02e55ee21559710975e849d79091dcf5d27bc8db1f1219f53003149bd879e`，板端 CRC32 `d98e6b16`。测试日志为 `build/regba-bringup-20261016-second.log`。

- 栈溢出的原因是 ARM/Thumb 翻译器每层栈帧约 10472/10440 字节。派生核心将代码发射和退出判断使用的递归深度统一限制为 8，同时将核心任务栈改为 240KiB；保留深度峰值与栈余量诊断。
- 同一 `/gba_game/test1.gba` 为 33554432 字节，载入后剩余堆 14653656 字节。`run 120` 完成，耗时 2013521µs、RGB555 CRC32 `1ce3f149`，翻译深度峰值 8，栈最小剩余 160760/245760 字节。随后 `run 0`、`keys 008`、`keys 000` 和 `stop` 完成，累计该次运行 541 帧、耗时 9062848µs；没有复现栈溢出。此为无屏短时运行，不代表游戏画面、实体输入或完整兼容性已验收。
- 重启后对 `/GBA/TEST/SDTEST00.BIN` 执行 CRC：131072 字节、`29df376d`，与首轮写入一致。本轮当时尚未比较 32MiB ROM 的电脑端 CRC，后续匹配结果见“电脑端 ROM 校验记录”。

第三版载入日志为 `build/ramload-20261016-112734-48464f.log`，测试日志为 `build/regba-bringup-20261016-third.log`。实板 FCSR 写入 0～3 后读回固定包含 `0x000c0000`；探针改为先原子记录完整硬件读回值，再跨抢占比较完整值，未屏蔽上下文检查位。`rtos-test` 通过：`context=0/0 rounds=32/32 preemptions=102/101 post_oom_heartbeats=49`。音频此版仍因 MDIV 读回检查失败，没有播放。

第四版载入日志为 `build/ramload-20261016-113816-f74b3c.log`，BIN 707952 字节、SHA256 `b10b396650921ad92b32fb382ba85b32378db594eb4eeedf4f08fac6f5456485`，板端 CRC32 `ef047501`。日志确认先检查正常 CPU 1.152GHz / MPLL 1.8GHz / DDR 600MHz，再经 YMODEM 完整传输、长度/CRC 校验并跳转 RAM；未写 NOR。命令记录为 `build/regba-bringup-20261016-fourth.log`。

- 音频两处问题均为寄存器读回条件误判：I2S 自动 D 计算后的读回 `0xbb`，以及 MDIV 字段对奇数取偶导致读回 0。修订检查后 DMA 正常运行。
- 当时版本 `tone` 播放 60 秒、1kHz。用户确认听到稳定连续的声音；完整播放后 `DMA_done=6000`，`FIFO_under=0 errors=0 timeouts=0`。`queue_under=1` 只发生在测试结尾数据耗尽，不是播放期间连续欠载的证据。
- 另一次播放期间显式 `rwtest` 新建 `/GBA/TEST/SDTEST02.BIN`，131072 字节、CRC32 `29df376d`。写入前后音频都为 `running=1`，DMA 完成数从 274 增至 884，队列欠载/FIFO 欠载/错误/超时均为 0，取得一次播放并行写卡证据。
- `jit-test` 同地址重写 `37 -> 73` 跨 tick 再次通过；`core-test` 120 帧耗时 2013542µs、画面 CRC32 `19b3e686`、六个 mailbox 正确、VBlank IRQ 118，全部检查通过。
- `rtos-test` 再次通过：32/32 轮、抢占 102/101 次、OOM 后心跳 49，ISR 栈剩余 16244 字节。

用户反馈长测试音刺耳后，代码已改为 2 秒、500Hz、峰值 1024/32768（约 -30dBFS），首尾各 50ms 线性渐变，并让 `stop` / `audio off` 取消测试音、关闭输出。静态复核还修正了已排队测试音的取消竞态：仅在接受新命令的临界区清除取消标志，工作任务不会覆盖随后收到的停止请求。该修订随后随第五版加载到 RAM；未重复执行 `tone`，不能把第四版的音频实测当作短音和取消行为的实板验证。

### 第五版：应用堆 DDR、清缓存重译及无声检查

第五版载入日志为 `build/ramload-20261016-120754-7b8b49.log`，BIN 711264 字节、SHA256 `881fe00866977d522871a1b008a644daf6c385cbf9eba3ce79c7ad3128493549`、板端 CRC32 `c84cada3`。YMODEM 长度和 CRC 校验后从 RAM 启动，未写 NOR。命令记录为 `build/regba-bringup-20261016-fifth.log`。Release 和 Debug 均编译并通过产物检查：Release 静态布局后堆 48500736 字节；Debug BIN 1325648 字节、堆 47886336 字节，完整哈希见对应 `manifest.json`。

- `ram-test` 独占分配应用堆中的连续块，保留约 2MiB 空间，经 uncached 别名读写真实 DDR。范围为 cached `[0x81206580,0x83dfff80)` / uncached `[0xa1206580,0xa3dfff80)`，共 46111232 字节；六种模式及 64 项 walking 检查通过，耗时 14096342µs。该测试不覆盖低位保留内存、当前代码/数据/任务栈或整个 64MiB，不替代全容量与边界验收。
- `rtos-test` 再次通过：32/32 轮、抢占 102/101 次、OOM 后心跳 49、ISR 栈剩余 16252 字节；`jit-test` 同地址重写 `37 -> 73` 跨 tick 通过。
- `core-test` 先执行 120 帧，再以 `TRANSLATION_REGION_READONLY / FLUSH_REASON_FULL_CACHE` 清空 ROM/BIOS 翻译及级联的可写缓存，在同一客体状态继续 120 帧。两轮耗时分别为 2012892µs、2013565µs，画面 CRC32 均为 `19b3e686`，全部像素检查通过，VBlank IRQ 从 118 累计到 238，打印 `core-retranslate-test PASS`。第二轮延续已初始化的客体状态；mailbox 保持预期值，不表示重新覆盖全部初始化路径。
- 重启后再次读取 `/GBA/TEST/SDTEST00.BIN`，131072 字节、CRC32 `29df376d`，与最初写入结果一致。
- `audio silent` 已加载并设置 `muted=1`。它强制 codec DAC 静音并拉低 PE17，保留 PCM 生成、转换、队列及 DMA；`status` 新增 `muted`、非零输入帧数 `nonzero` 和输入峰值 `peak`。
- 实际 `/gba_game/test1.gba` 静音短测运行 1379 帧、23095366µs，翻译深度峰值 8，栈最小剩余 160336/245760 字节，收到 `stop` 后正常退出。开始未按键时 PCM 为 0；串口 Start/释放（`keys 008` / `keys 000`）后，第 1260 帧统计为 `nonzero=759312 peak=23952`、`in=928747 out=1010880 DMA_done=2102 muted=1`，确认真实游戏已生成非零音频并走过 DMA。但当时 `queue_under=2 FIFO_under=3 errors=0 timeouts=0`，已停止运行并进入修正，不将其标记为游戏音频或 30 分钟压力通过。

欠载定位到串口任务的优先级与忙等发送：旧 console 优先级 4 高于核心任务的 3，一次约 550～650 字节的 `status` 输出在 115200 波特率下占用约 48～56ms，超过原先 4×10ms 队列可支撑的时间。修订源码将 console 改为与核心相同的优先级 3，DMA 环增为 6×10ms、启动预填充 4×10ms，并在清缓存重译检查中明确断言第二轮 IRQ 计数必须增加。修订版通过 Release/Debug 编译与产物检查：Release BIN 711376 字节、堆 48500736 字节；Debug BIN 1325792 字节、堆 47886336 字节。第六版已加载到 RAM，后续一分钟和 30 分钟游戏复测中均未再出现队列/FIFO 欠载，具体证据如下。

### 第六版：调度和音频队列修订

载入日志为 `build/ramload-20261016-122140-6762a0.log`，BIN 711376 字节、SHA256 `9509d0d3e7fecc827375a5ba06d5e3674a12b14096e7a809560fcfdf1f9a2acb`、板端 CRC32 `ecbdff41`。经长度和 CRC 校验从 RAM 启动，未写 NOR。命令日志为 `build/regba-bringup-20261016-sixth.log`：

- `rtos-test` 通过 32/32 轮、抢占 102/101 次、ISR 栈剩余 16252 字节。
- `core-test` 清缓存前后各 120 帧，耗时 2012942µs / 2013502µs，画面 CRC32 均为 `19b3e686`；IRQ 118→238，包含 IRQ 增长的显式断言通过。
- TF/FAT32 再次挂载成功。游戏静音短测使用 `scripts/soak-regba.py --duration 60 --press-start`，日志为 `build/regba-soak-20261016-sixth-short.log`，实际通过 60.062 秒稳态检查，不含热身。结束前为 3881 帧，音频 `in=2863616 out=3116860 DMA_done=6490 nonzero=2610488 peak=32768`；队列欠载、FIFO 欠载、错误和超时均 0。清理后为 3883 帧、DMA 完成 6495，核心和音频 `running=0`、`muted=1`，上述四项计数仍为 0。该结果说明本次修订在这一分钟内消除了第五版复现的欠载，不替代长时间测试或声音质量验收。
- 30 分钟静音游戏/PCM/DMA/TF 压力实际通过，日志为 `build/regba-soak-20261016-sixth-30min.log`；主机执行退出码 0，打印 `SOAK 30min PASS: actual=1800.094s; requested=1800s`，板端计时 1800.060 秒。计时不含热身，全程使用 `muted=1`，没有播放测试音。

长测最终结果：

| 项目 | 实际结果 |
|---|---|
| 结束前运行状态 | 107647 帧；31 个运行样本的首末帧增量为 107513，约 59.727 帧/秒。音频 `in=79480320 out=86509191 DMA_done=180224 nonzero=77286420 peak=32768`；核心/音频均运行，`muted=1`，DMA 稳态约 100 周期/秒 |
| 运行中音频计数 | `queue_under=0 FIFO_under=0 errors=0 timeouts=0`，每分钟采样均满足要求 |
| 并行 TF 写读 | 第 5、10、15、20、25 分钟新建 SDTEST03～SDTEST07，五个文件各 131072 字节，写入/同步/读回 CRC32 均为 `29df376d` |
| 既有文件读回 | 第 10、20 分钟读取 SDTEST00，131072 字节、CRC32 `29df376d` 均一致 |
| 内存与栈 | 稳态首末堆均为 14644280 字节，核心栈最小剩余均为 160312/245760 字节，翻译深度峰值 8；未出现异常或栈越界 |
| 时间与回绕 | 板端有效 1800.059960 秒 / 主机 1800.094 秒，差约 34.04ms；OST 微秒与 RTOS tick 持续增长，跨越两次 OST 低 32 位计数回绕 |
| 停止确认 | 107650 帧正常停止，音频 `in=79482624 out=86511699 DMA_done=180229 nonzero=77288724`；核心/音频 `running=0`，队列 0、`muted=1`，四项音频计数仍为 0 |

两次回绕的跨界采样为：715.827883 秒附近，`time_us=694469673 tick=694350` → `time_us=754203627 tick=754074`；1431.655766 秒附近，`time_us=1414415644 tick=1414176` → `time_us=1476244946 tick=1475995`。本次结果覆盖这一份 32MiB ROM 的无屏、静音运行与并行文件读写，未验收游戏可闻声音质量、显示/实体按键或广泛游戏兼容性。

新增 `scripts/soak-regba.py` 供已启动、已挂载的固件进行可复现的无声压力测试。默认先确认停止状态，设置并核实 `muted=1`，载入 ROM 后运行；`--press-start` 模拟 Start/释放，以进入产生非零 PCM 的游戏状态。ROM 载入默认等待上限为 600 秒，已载入且停止时可用 `--skip-load`。计时不含热身，每 60 秒检查状态、每 300 秒写读新 128KiB 文件、每 600 秒复核 SDTEST00；运行中要求队列欠载、FIFO 欠载、错误和超时全部为 0，并要求时钟、帧数、PCM 和 DMA 继续增长。最新脚本还比较 OST 微秒与 RTOS tick 增量，结尾比较板端时间和主机单调时间。结束或失败均发送 `stop` / `audio off` 并确认停止。脚本存在或短测通过不等于已完成 30 分钟验收。

```powershell
.\.venv\Scripts\python.exe scripts\soak-regba.py --duration 1800 --press-start
```

待完成项包括 DDR 全容量与边界、连续冷启动、游戏可闻声音质量、更多游戏兼容性、实际画面/实体输入及 `.sav` 保存后重载往返。短音及取消行为未进行声音验证；未进行 ReGBA NOR 固化。

## 电脑端 ROM 校验记录

2026-10-16，用户提供同一 ROM 的电脑端路径 `E:/gba/test1.gba`。实际只读计算结果如下，与已保存的板端 `crc /gba_game/test1.gba` 输出比较，字节数与 CRC32 均一致，阶段 2 的 32MiB 文件电脑端 CRC 对比通过；未改写 ROM、TF 卡或固件。

| 位置 | 字节数 | CRC32 |
|---|---:|---|
| 电脑端 `E:/gba/test1.gba` | 33554432 | `fc7b376f` |
| TF 板端 `/gba_game/test1.gba` | 33554432 | `fc7b376f` |

电脑端 SHA256 为 `ecd9bf6a2abc66866f679dd4df3bb0e78f218b2492610d399436be56e6547c61`，用于记录原文件身份，板端本次未计算 SHA256。对比证据保存于 `build/rom-integrity/20261016-test1.json`，板端原始输出见 `build/regba-bringup-20261016.log:67`。

## 初次板上验证顺序

两遍一致的完整 NOR 备份、RAM 首启及上述首轮项目已完成。以下为后续逐项复核参考顺序，不能将整套命令的预期结果视为实际通过。软件不自动挂载、写卡、载入游戏或播放声音；测试音只在需要听音确认时手动触发，本轮不再重复播放。

```text
status
ram-test
rtos-test
jit-test
core-test
mount
ls /
prepare
ls /GBA/ROMS
crc /GBA/ROMS/x2600-selftest.gba
rwtest
load /GBA/ROMS/game.gba
run 120
status
```

逐条等待完成输出；不要把整段一次发送。`ram-test` 只检查其独占分配的应用堆，建议在载入 ROM 前执行。`core-test` 不要求 TF 先挂载；运行内嵌测试 ROM 的 120 帧，清空 JIT 缓存后从同一客体状态再运行 120 帧，比对 ARM/Thumb/SWI/IRQ/RAM-code/SMC 检查值与完整 240×160 像素结果。`run 0` 持续运行，`stop` 在完整帧边界退出。当前 RAM 第六版 `tone` 已为 2 秒低幅度测试音，`stop` / `audio off` 可取消，但未进行声音验证；本轮不执行 `tone`。无声游戏检查先执行 `audio silent`，其 PCM 和 DMA 路径仍运行。

`rwtest` 会显式新建 128KiB 测试文件，保留文件用于重启后 CRC 复核。`prepare` 只补缺失目录。ROM 使用原始 `.gba`，首轮文件名用英文/ASCII。`save` 是显式 `.sav` 写入入口；自动存档、掉电提交策略、即时存档和 RTC 属于后续阶段。

现有 720×1280 MIPI 竖屏、3 倍 720×480 游戏显示及实体按键在阶段 5、6 接入，本轮没有修改屏幕驱动。用户提供的屏幕型号为“应该是 TFT FW050”，实际绑定尚待核实。Wi-Fi 子板保持断开；PC25～PC30 留给后续 MSC1 Wi-Fi。

## 阶段 5 的早期只读准备依据（历史）

已只读检查本轮备份中的原系统：NOR 偏移 `0x40000` 是 Linux 5.10.186 uImage，数据 CRC32 `561bdeb6`；gzip 解压内核 13145632 字节，其中偏移 `0xC78260` 的 DTB 为 28371 字节、CRC32 `e7b63d61`。DTB 的板型为 `ingenic,x2600-halley7`，DPU 为 `okay`、旋转为 0，前两层配置均为 720×1280、各两个帧缓冲，并有 lane2/3 PN swap 配置。FW050 与 `zc50289hshd02` 节点虽然都为 `okay`，DPU 的 `remote-endpoint=0x2b` 实际指向 ZC502；FW050 endpoint 没有 phandle，不能据此认定 FW050 是原系统实际绑定的屏幕。

两个 panel 的供电/复位均为 PC11/PC10，背光为 PWM15_PC14，与 `u-boot-ingenic/include/configs/x2600m_halley7.h` 一致；随包 MIPI V1 图上的 PC08/PC15/PE02 属于另一版本，不能直接套用。`libbare-cpu/projects/x2660-halley/Examples/lcd-mipi-fw050/fw050.c` 的 `power_on` 使用 PC11/PC10，而 `power_off` 使用 PC08/PC15，样例内部有冲突。命令表候选还包括 `u-boot-ingenic/drivers/video/jz_lcd/lcd_panel/lcd_zc502.c`。阶段 5 应先在原 Linux 只读核对实际 panel 绑定、lane 和时钟，再选择初始化命令表；本轮只核对备份内容和本地源码，没有生成拆包产物或改动屏幕驱动。

# HALLEY7-M / X2600M ReGBA

本目录是当前掌机应用：MIPS 主核 FreeRTOS、ReGBA / MIPS JIT、SPI TF / FAT32、竖屏 MIPI、内置音频、原生存档、板载按键、UART 蓝牙手柄和中文 GUI。新克隆部署方式见[仓库 README](../../README.md)，在仓库根目录运行 `scripts/build-regba.ps1` 构建，运行 `scripts/test-regba-host.ps1` 做主机回归；NOR 安装另见[烧录步骤](../../docs/烧录步骤.md)。

```powershell
git clone https://github.com/PKM-Bai/x2600.git
cd x2600
git submodule update --init libbare-cpu third_party/ReGBA
```

工具链使用官方 XBurst2 ELF R5.1.6，本机安装方式、Python 依赖和 CMake / Ninja 准备见仓库 README。可选的 `u-boot-ingenic` 子模块只在重编原启动器或复现早期 Hello 流程时初始化。代码按现有功能边界分组导入，集成提交之前的中间提交不保证独立构建，也不代表开发当时的历史快照。

mGBA 独立版本固定为 0.10.5。保留上述两个公共依赖后额外执行：

```powershell
git submodule update --init third_party/mGBA
.\scripts\build-regba.ps1 -Engine mgba
.\scripts\build-regba.ps1 -Engine mgba -Configuration Debug
```

输出分别位于 `build/mgba-Release` 和 `build/mgba-Debug`，文件名继续为 `halley7m_regba.bin` / `.elf`，以复用现有 RAM 加载工具。此版本只链接 mGBA 的 GBA 解释器、软件渲染和音频核心，使用内置 HLE BIOS，原生存档在 `/GBA/SAVES/mGBA`；不读取 ReGBA 的 `game_config.txt` 或外部 BIOS。`test-save-roundtrip.py` 和 `test-engine-gui.py` 验证该版本时传 `--save-dir /GBA/SAVES/mGBA`。默认 ReGBA 的构建目录、存档目录和命令保持原样；具体软件及实板放行范围见[引擎分离实施记录](../../docs/ReGBA引擎分离实施记录.md)。

纯软件检查使用 `python scripts/test-mgba-sim.py --mode cpu --timeout 360`，还可分别选择 `sram`、`flash`、`eeprom`、`mirror`。脚本编译真实适配层，使用已有官方 Linux 工具链的 MIPS 模拟器；Windows 下通过 WSL 执行，不打开开发板串口。Flash 模式可用 `--timeout 900` 留出模拟器执行时间。mGBA 的 EEPROM 诊断采用 DMA3 事务，Flash 诊断等待编程完成，二者保持相同邮箱与完整原生存档校验；默认 ReGBA 诊断不变。模拟器耗时不用于推算开发板帧率。

本文保留阶段 0～7 及后续 GUI 各版记录。`build/` 中的原始日志、镜像、ROM / 存档及备份仅在开发机保留；在线摘要见 [docs/validation](../../docs/validation/)，其中的原始路径用于追溯。板级原理图 / BOM、工具链和 Cloner 需单独准备。

当前 NOR 为 Release **2306352 字节 / CRC32 `178853ea`**，所有 GUI 页面（含游戏设置）继续使用 **GBA**。游戏中 Y 打开七项菜单：继续游戏、保存进度、读取存档、游戏设置、切换游戏、资源路径、退出游戏。“读取存档”确认后读取原生 `.sav` 并复用缓存 ROM 重启，再在游戏内选择继续游戏；读取失败保留当前 guest。进入菜单只等待在途保存，不新提交当前进度，显式保存 / 正常切换 / 退出仍完成同步。

游戏设置：音量 0～100、每次 10（0 关闭功放），跳帧 0～3，速度 1× / 2× / 4×；上下选择，左右或 A 调整，B / Y 返回。设置只在会话内保留，不落卡，默认音量 100 / 跳帧 0 / 速度 1×；加速静音，回 1× 恢复声音。双构建、完整主机回归、RAM GUI、SRAM / Flash / EEPROM 的保存 A → 未保存 B → GUI 读取恢复 A、32MiB 缓存读档、NOR 全字节 / 环境 / 保护区核验和软件复位自动进入列表均通过，见[本次摘要](../../docs/validation/gui-options-20261018-validation.json)。用户确认“菜单、音量、加速和声音恢复均正常”；本版未做冷启动、30 分钟长测或真实游戏具体剧情点恢复。`c77de25d` 标题统一版和下面中文文件名版的验收保留各自历史范围。

中文 ROM 文件名验收基线为 Release **2301840 字节 / CRC32 `a85324e3`**、Debug 构建 2955712 字节。FatFs 使用 UTF-16 长文件名，应用 / GUI 使用 UTF-8，由文件任务严格转换；字库包含仓库 Unifont 的 34869 个可用 BMP 字形，覆盖常用简繁中文，缺字仍显示方框。文件名单项上限 255 UTF-8 字节，文件层路径上限 512 字节，ROM 启动入口当前上限 511 字节；非法编码或超长路径报错、不截断后访问。双构建和完整主机回归 `build/chinese-name-host-20261018.log` 通过，用户确认 RAM 列表中文正常；当前已从旧 `3d721f71` NOR 更新，全部程序字节 / 环境 / bootcmd 核验和软件复位自动进入新 GUI 通过，8 个中文 ROM 名均正确读出。本版已通过实际 GUI 的《龙珠大冒险》→32MiB《究极绿宝石-v5.4》→龙珠→退出，SAVE 3/3 无错误，中文 `.sav` / `.bak` 均 32768 字节、CRC `a2d2225d`；32MiB 运行剩余堆 5526136 字节，HUD 约 59.5～59.7fps，14 个状态样本音频四项错误和 LCD 欠载为 0。该窗口由串口驱动 GUI、手柄未连接。冷启动自动检查再次读出 8 个中文名，存档 / 备份 CRC 保持一致；用户已按要求断电约 5 秒再上电，核对中文列表、龙珠加载页中文标题和游戏画面，对本版这四项统一回复“正常”；本版未新增物理手柄或音质专项验收；进展见[版本摘要](../../docs/validation/chinese-name-20261018-validation.json)。

2026-10-18 初版中文掌机 GUI 历史：开机进入游戏库，A 启动 / 确认、B 返回、方向键选择、L/R 翻页、SELECT 切换标准 / legacy 目录、START 查看资源路径，当时游戏中按 Y 打开继续 / 保存 / 切换 / 资源 / 声音 / 退出菜单。用户确认中文列表正常；内置完整兼容配置表（TF 匹配项优先）解决了缺少 128KB Flash 配置导致的 Pokémon 纯白，SPI 数据时钟从 3.75 提至 7.03125MHz，初始化仍 375kHz。`65b3e377` 修正版已实板运行：Pokémon 加载 15.61656 秒、test4 加载 27.09714 秒，用户确认两个游戏正常、UI 显示保存成功；完整 ROM CRC、SDTEST24 写入同步读回、两种存档完整 CRC、切换与退出均通过，SAVE 4/4 错误 0，音频四错误及 LCD / 蓝牙错误为 0。

该轮也发现切换到 32MiB `test1.gba` 两次 ENOMEM：首次运行后才创建的音频常驻任务分割了连续空闲堆。GUI 基线修订仅将 GUI 首次音频初始化提前到首 ROM 前，保持静默；Release 912416 / Debug 1563168 字节、完整主机回归及同一 newlib 分配顺序对照通过。**GUI 基线 Release 已按 CRC32 `1f242dd3` RAM 校验启动，通过 8MiB → test4 → 32MiB → 8MiB、三次保存与正常退出**；加载依次为 15.878107 / 27.061455 / 63.219320 / 15.880769 秒，SAVE 3/3 错误 0，两种存档完整 CRC 与快照匹配，退出后 loaded=0、空闲堆 40470152 字节，音频四错误 / LCD 欠载为 0。本轮用串口输入驱动实际 GUI，蓝牙未连接，物理手柄 / 人眼确认仍属于前版。保存同步后才允许切换 / 卸载，失败保留 RAM 进度并提示重试；实际游戏进度恢复、失败重试与 GUI 长测仍待验收。该历史基线仅有中文标签，CP437 超出字符集时回退 SFN；当前中文文件名修订见本文顶部。操作、当前产物 SHA256、日志及验证范围见[掌机 GUI 实施记录](../../docs/ReGBA掌机GUI实施记录.md)。

GUI 基线的独立板测摘要见 [gui-final-20261018-validation.json](../../docs/validation/gui-final-20261018-validation.json)；构建 manifest 的 `hardware_validated=false` 保留生成器原值，实板结论由独立摘要和原始日志记录。

SPI 优化历史基线在保留 SPI 与 UART2 原接线的条件下完成软件提速：数据时钟目标 18.75MHz 并受 CSD 上限限制，读取采用 CMD18 / CMD12、CRC16 查表、512 字节交换和 64 字符在途上限，GUI 进度约 10Hz；派生 SSI HAL 的超时轮询改用 1kHz RTOS tick。该历史基线 Release **913680 字节 / CRC32 `83547b73`**、Debug 1564608 字节，双构建 / 产物检查、完整主机回归、Release RAM 和实板复验均通过。8MiB 两次加载 **7.369 / 7.388 秒**、test4 **12.369 秒**、32MiB 两次 **29.292 / 29.348 秒**；加载速度约为 GUI 基线的 2.2 倍、耗时减少约 54%，**未达到 5 秒目标**。三个 ROM 完整 CRC、游戏运行中两次 128KiB 写入同步读回、SAVE 3/3 且两种存档完整回读、正常切换 / 退出均通过，音频四错误 / LCD 欠载为 0。该轮仍由串口驱动 GUI，未做物理手柄 / 实际剧情恢复 / 当前版 30 分钟长测；板端回游戏列表，COM52 已释放，全程只加载 RAM、未写 NOR。独立摘要见 [spi-fasttick-20261018-validation.json](../../docs/validation/spi-fasttick-20261018-validation.json)。

旧 HUD NOR 版 `3d721f71` 的历史记录：游戏上方黑边显示 **CPU / FPS**，每秒更新一位小数。CPU 为全系统非 Idle 占比，FPS 为实际完成的 GBA 帧率，不是约 30Hz 的 LCD 刷新率；暂停结束采样、恢复重新计窗，无新任务或动态分配。Release **915536 字节 / CRC32 `3d721f71`**、Debug 1567328 字节，双构建、完整主机回归 `build/hud-nor-host-final-20261018.log` 和 RAM 检查通过。test4 HUD CPU 约 10.1%、FPS 59.8，独立 5 秒忙碌 9.57% / Idle 90.43%；暂停约 2 秒冻结、恢复重计、切换 Pokémon、TF 写读和正常退出通过，音频四错误 / LCD 欠载为 0。用户已授权首次 NOR 安装，使用[串口首装脚本](../../scripts/flash-regba-nor.py)，不是旧 Cloner 策略；本版 NOR 写入、独立完整回读和软件 `reset` 自动启动均通过，原 SPL / U-Boot 代码 CRC `3a3c3918`、本次 rootfs CRC `8618c73f` 保持不变；NOR 启动后完整 HUD / 切换 / TF 写读检查也通过。真实断电上电已捕获自动启动链，用户确认“已断电上电，CPU/FPS显示清晰，游戏和手柄正常”。冷复合监测因连接前 UART 错误 1 主动退出，其 JSON 的 pass=false 不能解释为冷启动失败；随后三次 test4 实际游玩 CPU 15.2 / 13.5 / 16.0%、FPS 59.8，UART 1、音频队列 / FIFO 欠载 1 / 1 均稳定未增，DMA 错误 / 超时及 LCD 欠载为 0；与上述 RAM / 软件复位全零窗口分开记录，未扩展音频专项。当前固件未重做 32MiB、GUI 30 分钟长测或实际剧情存档恢复，test4 保持有声运行，COM52 已释放。聚合证据见 [hud-nor-20261018-validation.json](../../docs/validation/hud-nor-20261018-validation.json)。该次首装在擦除前核对启动代码 / 环境 / kernel 与原双备份匹配，rootfs 记录并保持操作前 CRC；不带 `--previous` 的首装模式会拒绝重复安装。当前脚本另支持显式指定已装镜像的同范围 / 扩大范围更新，中文名新版已完成该 NOR 更新和软件复位，游戏 / 存档和冷启动自动检查已通过，新版断电约 5 秒再上电、中文列表、龙珠加载页中文标题和游戏画面均已获用户“正常”确认，规则见烧录步骤。首次 rootfs 历史差异预检在擦除前停止，修订后的独立模型 `build/hud-nor-flash-host-final-20261018.log` 已通过，写入见 `build/flash-regba-nor-20261018-resume.log`，最终有效回读 / 复位证据见 flash-regba-nor-20261018-verify.json（本地记录 `build/flash-regba-nor-20261018-verify.json`），NOR 启动后检查见 `build/hud-nor-boot-20261018.log` / `.json`。详见[烧录步骤](../../docs/烧录步骤.md)。

蓝牙接线仍为 UART2 RX / PD12 FUNCTION0 / J10 pin2：橙 TXD→J10 pin2、红 VCC→J3 pin14、棕 GND→J3 pin10，RXD 空、TF 原线不动，与 J10 WiFi 子板互斥。GUI 前的基线 `0f1c6177` 已通过有声 / 显示 / TF 的 30 分钟压力（主机 1800.243 秒 / 板端 1800.210 秒，音频四错误和 LCD 欠载为 0）；蓝牙早期有新输入后空闲断联，并非持续 30 分钟手柄操作。音频专项调试已按用户要求撤除，默认增益恢复 200，音质根因未测定；十字右上输出受限、左摇杆右上已过，静置无周期上报，timeout 保持 0。该蓝牙基线全程未写 NOR，接线与基线日志见[蓝牙手柄接入说明](../../docs/ReGBA蓝牙手柄接入.md)。

本工程实现 X2600M 主核 FreeRTOS、ReGBA 核心、SPI TF/FatFs、内置音频和阶段 5 的 MIPI 显示后端。用户提供的屏幕标称为 **TFT FW050**，实物背标和厂家型号尚未核实；原 Linux 实际绑定 `zc50289hshd02` 驱动，当前移植沿用其初始化命令和时序，模式为 720×1280p30。驱动绑定不能直接证明实物型号，也不足以否定 FW050。屏幕保持原生竖屏，240×160 游戏帧按 3 倍最近邻缩放到 720×480，位于 `(0,400)`，上下各保留 400 行黑边。本轮阶段 6 接入底板三枚用户键：SW6/PE13 为 Start，SW3/PE14 为 A，SW4/PE15 为 B；实板结果见[板载按键实施记录](../../docs/ReGBA阶段6板载按键实施记录.md)。

2026-10-18 GUI 前 H 历史结果：H 在真实 `test4.gba` 上通过静音启停、运行态完整 ROM CRC、手动原生存档 / TF 读回和带屏、非零 PCM、TF 并行读写的 30 分钟联合压力。主机 1800.188 秒 / 板端 1800.154975 秒，音频四错误与 LCD 欠载均为 0，用户确认画面正常。三类诊断存档往返与普通复位后恢复也已完成。长测全日志 35 份完整状态的 SAVE 均为 1/1、错误 0、无忙碌或待保存修改；这 1 次提交来自测前手动保存，长测期间没有新增原生提交。新的游戏内保存点恢复与声音听感仍待专项验证。结束后核心 / 音频停止静音、LCD 保留画面，当时 H 仍在 RAM，未写 NOR。完整依据见[存档与音频实施记录](../../docs/ReGBA存档与音频实施记录.md)。

2026-10-17 实施起点与版本 A 结果：原生 `.sav` 后台保存、最后一次修改后 2 秒自动提交、运行中手动保存、停止 / 切换 ROM flush、上一版本回退和坏档拒绝加载已实现；修复存档类型被兼容配置初始化清空、SRAM `0x5555` 写入漏标记，以及 64KiB SRAM / Flash 共用长度导致的重载直接写入问题。内置音频补充停止 / 入队竞争与启动取消修订，移除运行中的周期性栈日志，新增发布间隔与 DMA 重装时间诊断。版本 A 的 Release / Debug、完整主机回归及派生核心 MIPS 回归通过；RAM 校验启动后，三类诊断 ROM 的 2 秒自动保存、运行中手动覆盖、立即停止刷新、逐步重载、完整 TF CRC 及普通复位后只读恢复均通过。复位后仍为 A / count 5，且本次启动保存计数保持 0；诊断 ROM 静音短测音频四项错误与 LCD 欠载均为 0。

后续版本 B 增加两帧之间快速 off→silent 的停止标记同步、启动取消后保留最新请求、整体停机时禁止再启动音频。B Release 840496 字节 / Debug 1479984 字节，构建及产物检查通过，Release 已按 CRC32 `5db134fe` 校验后加载 RAM。B 独立重做三类存档自动 / 手动 / 停止保存和重载，最终均为 A / count 8；真实 `test4.gba` 的首次取消、5 轮暂停恢复、5 轮快速 off→silent、稳定等待期间改请求与 stop/run 均通过，非零 PCM 已确认，音频四错误与 LCD 欠载全为 0。B 的 30 分钟带屏非零 PCM / TF 联合压力在首个 300 秒 `rwtest` 返回 EIO 后中止，未通过；该轮停止核心 / 音频并保持静音，后续按版本逐步定位 TF 错误。商业游戏实际保存点恢复和听感仍待专项确认。以下旧版本记录保留各自适用范围，当前 H 结果见上文及后续段落。

长测途中也已观测到真实 `test4.gba` 的后台存档完成：`/GBA/SAVES/test4.sav`、32768 字节、快照 CRC32 `a2d2225d`，B 本次启动累计保存 `10/10`、错误 0，无待保存修改。该证据确认真实游戏触发了保存，不替代实际保存点重载恢复验收。

EIO 后的停止态复查也曾在目录和 CRC 读取中返回 `-5`，随后新 SDTEST10 写读及目录读取恢复正常；`test4.sav` 实际读回 CRC32 为 `a2d2225d`，SDTEST09 / SDTEST10 / 历史 SDTEST00 均为 131072 字节、CRC32 `29df376d`。尚不能据现有日志定位失败阶段或协议原因；未见上述文件内容损坏，音频四错误与 LCD 欠载在中止时仍均为 0，不能记作长测通过。

用户确认 `test4.gba` 为《龙珠大冒险》，反馈画面卡住且不确定三键能否到达保存菜单。EIO 后压力脚本主动停止、后续静音恢复、再为诊断固件准备而主动停止均有记录，不能据画面静止直接确认异常死机。最后 B 运行 119.75 秒 / 7152 帧后按命令停止，存档 `10/10` 无错误，音频四错误与 LCD 欠载仍为 0；实际游戏进度恢复继续待验收。

版本 C 已按板端 CRC32 `f7146db3` 校验加载，启动后 TF 读取和 SDTEST11 写读通过；运行后 300 / 600 秒 SDTEST12 / 13 写读均通过，没有复现原 `rwtest` EIO，但音频队列 / FIFO 欠载各 1，不能记作联合无错误通过。随后游戏运行时整 ROM 读取捕获 `CMD17 read-token` EIO，静音 DMA 关闭后仍复现；核心停止后整 ROM 14357872 字节、CRC32 `2cf3dede`，与 PC 文件一致。音频 DMA 不是该错误发生的必要条件，当时物理 / 软件根因尚未定位。该轮结束时 C 正常停止、静音，详细 token / 扇区及日志见[存档与音频实施记录](../../docs/ReGBA存档与音频实施记录.md)。

版本 D 双构建与完整主机回归通过：Release 844272 字节 / 初始堆 48234496 字节，Debug 1485664 字节 / 初始堆 47591424 字节。D 保持 6 周期 DMA 容量，预填充由 4 改为 5 周期，并增加首欠载发布快照、核心 PCM 调用间隔、文件错误阶段及非法 SD token 现场诊断；不加入重试、不降频、不修改 SDK。D 已按板端 CRC32 `425aeb1c` 校验上传启动，但首次挂载和一次显式重新挂载均为 FatFs 3 / `-19`，尚未载入 ROM；核心 / 音频保持停止静音，LCD 开启正常。用户确认未改 TF、供电或杜邦线，当前不推定硬件损坏或接错线。

D2 仅补 `sdspi_initialize()` 失败出口的阶段、R7/OCR/CSD 等已有观测，双构建及产物检查通过：Release 845264 字节 / Debug 1486976 字节，初始堆分别为 48234496 / 47591424 字节。D2 已按 CRC32 `3de6405c` 校验启动，首次挂载及存档 CRC 正常；但尚未运行游戏的 ROM 加载阶段即捕获数据 CRC 错误，随后停止态打开文件所需的目录读超时，再初始化 CMD0 / R1 超时。该轮结束时 TF 未挂载、无 ROM 运行、音频停止静音，5 周期预填充未取得硬件验收。证据说明游戏运行与音频 DMA 都不是 TF 错误的必要条件；随后核对 HAL 清理和 END 等待时序形成 E，未据此判定唯一实板根因或卡损坏。30 分钟联合压力、商业游戏实际进度恢复与听感仍未通过。

E 已修订 SSI 完成时序：从 SDK 派生编译副本，轮询交换的软件 RX/TX 循环结束后先等待 END，再执行正常接收关闭 / FIFO 清理，复用原超时与错误解锁；本地空闲判断要求同一状态采样 `END=1 && !BUSY`。依据 PM，BUSY 为 0 可能仍是 END 为 0 的 interval；软件对照测试中原始函数按预期失败、派生函数通过。原 SDK 保持干净，TF 仍 3.75MHz、无重试和中断屏蔽，音频容量 / 5 周期预填充不变。E 已按板端 CRC32 `bb995465` 校验启动，但首次挂载 CMD0 超时；用户保持板供电 / 线束 / RAM E，仅 TF 拔出等 5 秒再插回后，CMD0 成功但 CMD8 超时，单次复查 CMD0 又返回与预期不同的 R1。该轮 TF 未恢复稳定，不能记为 E 修复通过；结束时无 ROM 运行、核心 / 音频停止静音。

最新硬件核查发现卡座 VDD 杜邦线已掉落，用户现已接牢 VDD → J9 pin14、GND → pin6；底板原理图第 6 页确认 pin14 经 0Ω R88 接 3V3P / C110 去耦，pin6 为 GND，接法正确。用户无万用表，实际电压无实测；后续确认杜邦线约 5cm。同一 RAM E 在接牢后单次挂载仍 CMD0 超时；用户随后再次单独拔出 TF 5 秒重插、不复位，挂载仍为相同超时，TF 尚未通过。此前多轮异常存在此供电连接外部条件，不能把 SSI 或 E 变更认定为唯一实板根因。

E2 的最小 `tf-probe` 由文件任务在未挂载、无打开文件时执行约 100kHz GPIO mode 0 的 8 次 CMD0→CMD8/R7 试验，仅发送诊断命令、不读写文件；正常 mount / 3.75MHz / END 处理保持原样。Release 847040 字节 / 初始堆 48230400 字节，Debug 1489520 字节 / 初始堆 47587328 字节，双构建及检查通过，Release 已按 CRC32 `576ed3be` 校验上传启动。用户确认 CS7 / CLK9 / MOSI5 / MISO3 四线正确、线长约 5cm 后，同一 RAM E2 再次 GPIO 8 轮均无有效 R1（`0xFF` 为占位，不证明 MISO 恒高），SSI 挂载仍 CMD0 超时、FatFs 3 / `-19`，日志 `build/regba-E2-wiring-confirmed-20261017.log`。该次结束时无 ROM、0 帧、存档 0/0、LCD 未开、音频停止静音，COM52 已关闭。绕过 SSI 后仍未收到有效 R1 不支持把此次故障归到 FatFs/音频/JIT，也不能单凭此判卡坏；GPIO 与 SSI 的实现和频率差异仍保留为对照限制。

USB 读卡器只读对照已完成：卡为 G: / USB disk2、MBR、31914983424 字节，FAT32 卷 31902400512 字节；另一槽 F: 为 NoMedia。`GBA` 与 `gba_game` 的 25 文件 / 62560624 字节已备份，错误 0、全部备份 SHA256 一致，见备份目录（本地记录 `build/tf-reader-audit/20261017-191623/files`）和清单（本地记录 `build/tf-reader-audit/20261017-191623/manifest.json`）。`test4.gba` CRC32 `2cf3dede` / `test4.sav` `a2d2225d`、SDTEST00～13 `29df376d` 及三类诊断档均与之前板端预期吻合，三类 A / count 8 的本地备份又通过完整数据逐字节校验（本地记录 `build/tf-reader-audit/20261017-191623/diagnostic-save-payload-verify.json`）。Windows 初始 Health Warning / FullRepairNeeded；无修复参数的只读 CHKDSK（本地记录 `build/tf-reader-audit/20261017-191623/chkdsk-readonly.txt`）开头报 Access denied，随后扫描完整、最终报告无问题且退出 0，不据此判定 Warning 来源或整卡健康；未修改卡文件、修复或格式化。

用户已安全弹出 G: 并回原 SPI 卡座，保持板供电 / 接线、不复位。`build/regba-E2-after-usb-reader-20261017.log` 再次 GPIO 8 / 8 无有效 R1，SSI CMD0 超时（failure 3、HAL 0 / state 1 / error 0 / SSI `0x98`）、FatFs 3 / `-19`；板端未恢复。该次复查结束时 E2 保留 RAM，核心 / 音频停止静音、0 帧 / 存档 0/0 / LCD 未开 / TF 未挂载，COM52 正常关闭；其后整板断电对照见下一段。USB 读取和存档数据通过不替代板端 SPI 稳定性验收。

用户已整板断电，仅把 TF VDD 从 J9 pin14 移至 J10 pin12 后正常上电，地和四 SPI 不变；无备用杜邦线、卡座或万用表。J10 pin12 为 3V3P，R88 在原理图 / BOM 中为常规 0Ω、非 NC。改供电引出点后起初仍失败；随后用户接牢 VDD、没有 RESET，同一 RAM E2 的 GPIO 8/8 与正常 SSI/FAT32 挂载恢复，四份存档 CRC 与停止态整 ROM CRC `2cf3dede` 均通过，LCD 开启通过。用户确认具体为 VDD 松动，没有报告针号新变化，最近供电记录仍为 J10 pin12 / 地 J9 pin6；不认定 R88 或特定引脚故障。

恢复后的 E2 静音音频生命周期未通过：首次 100ms 取消通过，首次 silent 恢复出现 FIFO 欠载 1、队列欠载 0，五轮切换尚未执行。随后关闭音频、保持游戏 / LCD 运行读整 ROM，在 73728 字节时 CMD17 `read-token` 返回 HAL 超时（hal 3 / state 5，区别于早先 hal 0 的非法 token）。核心仍响应并按命令停止；显式 `mount=-16` 是 ROM 文件仍开、没有重新初始化，同轮存档 CRC 恢复读取不记为 remount 或 reset 恢复。该轮结束时 E2 保留 RAM，核心 / 音频停止静音、LCD 开启；当时准备 SSI 完成计数优先于 deadline 的最小候选与首播 FIFO 实际 PCM 预填窗口，主机测试不替代实板修复，后续 F 至 H 构建 / 验证另记。当时新一轮长测尚未完成；实际游戏进度恢复与听感至今仍未验证。

F 保留 16 样本真实首播 FIFO 预填 / 464 样本首 DMA，并修订有限 deadline 顺序，已完成构建 / 主机回归及 RAM 校验。F 停止态完整 ROM `2cf3dede`、文件基线与 LCD 开启通过，但真实游戏生命周期在加载阶段就遇到 `tx_left=0 rx_left=1` 的 HAL 超时，音频未启动。首次内置诊断加载也遇异常 R1，后续 GPIO 不通过后紧接 SSI 挂载成功，中断后的协议状态仍不确定。

成功挂载后，F 以 `/__save_sram.gba` 完成 36 观察点的零 PCM 静音生命周期：首次取消、5 普通 + 5 快切、稳定等待最新请求及 stop/run 全通过，音频四错误 / LCD 欠载和首次欠载字段全为 0；DMA 90、输入 42337 / 输出 46080、`nonzero=0 peak=0`、存档 0/0 未写卡。该项只证明独立启停与零 PCM DMA 路径，不作为真实游戏非零 PCM、供给间隔重现、听感或联合长测通过。

G 将 SSI FINISH 移至 TX/RX 都完成后、END 等待前，并增加清理前快照；已按 847840 字节 / CRC32 `a4e730f3` 校验加载。但核心 / 音频 / LCD 均未运行时首次挂载仍失败：收发剩余计数均为 0，END 等待超时，清理前 SSI `0x5A` 为 FIFO 空、BUSY=1、END=0、TUNDR=1，UNFIN=0；这是区别于 F 剩余 RX=1 的新现场，见 `build/regba-G-boot-baseline-20261017.log`。存档未读、TF 未挂载，LCD 后续开启通过。

H 仅在 FINISH 后、END 等待前清 TX_ERROR，保留 ROVER 及数据数量 / 频率 / DR / 传输 / 中断 / 重试；双构建 / 模型回归通过。2026-10-17 的监听超时未上传，2026-10-18 同一 H 已按 847856 字节 / CRC32 `1e8f4906` 校验启动。真实 `test4.gba` 的 36 观察点静音生命周期通过、非零 PCM 已确认，音频四错误与 LCD 欠载均为 0；游戏 / LCD / DMA 运行时完整 ROM CRC `2cf3dede` 通过，真实手动保存 32768 字节 / `a2d2225d`、SAVE 1/1 无错误并 TF 读回一致。新的游戏内 checkpoint 恢复与听感仍未确认。

H 已完成 `--duration 1800 --skip-load --require-display` 的带屏、非零 PCM、TF 联合压力，日志 `build/regba-H-save-audio-soak-20261018-30min.log`，汇总 `build/regba-H-soak-summary-20261018.json`。正式 31 份运行状态为 134→107653 帧、约 59.72764fps，5 个新 128KiB 文件与 2 次旧文件 CRC 复核通过；音频四错误 / LCD 欠载为 0，空闲堆 26157688 字节、核心栈最小剩余 160280 字节稳定。独立复核包含准备 / 停止态的全部 35 份 SAVE / backup 状态，无保存错误或待提交修改，正式长测原生提交增量为 0。最终 `build/regba-H-final-save-audio-status-20261018.log` 再确认正常停止在 107660 帧、SAVE 1/1，`test4.sav` 32768 字节 / `a2d2225d`、最后新建 SDTEST18 131072 字节 / `29df376d` 均匹配，正式存档和 `.bak` 在、无 `.tmp`。用户确认画面正常；全程静音、未写 NOR。

本地 `E:/gba/test4.gba` 虽保留 `EEPROM_V124` 签名，保存库读写函数已通过 `LDRB` / `STRB` 直接访问 `0x0E000000` SRAM，总线实现符合 EEPROM→SRAM 补丁行为；32KiB `.sav` 与该访问方式一致。本轮保持类型，未修改 ROM，也不按签名强制 EEPROM；文件 SHA256、函数偏移及核查范围见实施记录。

代码和构建产物与实板验收分别记录。2026-10-16 已完成完整 NOR 双遍只读备份，并通过 U-Boot/YMODEM 在 RAM 启动 ReGBA。RTOS 上下文、46111232 字节应用堆 DDR、JIT、内置核心清缓存重译、TF 重启读回、32MiB ROM 电脑端 CRC 对比及内置音频首轮检查通过。第五版游戏欠载修订后，第六版通过 32MiB ROM、非零 PCM、静音 DMA 和并行 TF 读写的 30 分钟无屏压力：主机 1800.094 秒 / 板端 1800.060 秒，队列/FIFO 欠载、错误和超时均为 0，跨过两次 OST 低 32 位计数回绕，堆和栈余量稳定。该轮压力结束后核心/音频正常停止；所有 ReGBA 调试固件均仅加载 RAM，未写入 NOR。以上为 2026-10-16 的阶段 0～4 结果；当时尚未完成的存档往返已在后续 A / B 三类诊断及 H 真实文件保存 / 回读中取得证据，实际游戏新保存点恢复仍未验证。DDR 全容量与边界、多次冷启动、更多游戏兼容性 / 完整实体输入、可闻音质仍待验收，阶段 0～4 尚未全部完成。

阶段 5 的静态图、真实 ROM 画面、联合短测和显示停止/重开均已取得首轮实板证据。第三版补充面板复位释放后 120ms 等待，用户确认彩条及游戏画面正常；同版通过显示/静音音频/TF 的 320 秒联合短测。最终第四版补充 DSI 关闭与重开复位，首次开启及两轮重开均无 PHY 警告、帧缓冲内存完整回收，用户确认重开后彩条正常；随后再次通过 60 秒显示/静音游戏回归。该轮结束时核心和音频停止，音频静音、队列清空，LCD 保留最后画面，COM52 已释放；本阶段没有播放测试音、未写 NOR。2026-10-16 阶段 5 第四版尚未做带显示长压；后续 H 已完成本游戏的 30 分钟带屏联合压力，多次断电冷启动及更多游戏覆盖仍待完成。完整历史依据见[阶段 5 显示实施记录](../../docs/ReGBA阶段5显示实施记录.md)，当前结果见[存档与音频实施记录](../../docs/ReGBA存档与音频实施记录.md)。

用户反馈原 60 秒、1kHz 测试音刺耳后，代码已改为 2 秒低幅度短音并支持停止，Release/Debug 编译均通过，且已随第五版加载到 RAM。本轮未执行 `tone`，短音及取消行为尚未进行声音验证。新增 `audio silent` 可在 codec DAC 静音、PE17 功放关闭的同时运行真实 PCM、转换和 DMA 路径。后续默认静音检查，需要出声时先告知，只进行短时一次确认。

阶段 6 本轮按用户最新要求全程静音，不播放游戏声音或测试音。三枚用户键使用独立任务每 5ms 扫描、20ms 消抖，支持长按、释放和组合键；板载输入与串口 `keys` 输入按位或合并。SW1 USB Boot、SW5 eMMC Boot 和 SW2 RESET 保留各自功能。Release/Debug 构建及产物检查、生产消抖实现的主机回归已通过；Release 已经长度 / CRC32 校验后加载 RAM 并启动扫描任务。`test4.gba` 中用户确认三键都有响应，各键按下/释放计数增长、松开后掩码回零；物理长按、组合键及输入合并仍待逐项验证。本轮采样堆保持 26408736 字节，LCD 欠载 0；静音音频队列/FIFO 欠载各 1、错误/超时 0，不能记为音频零欠载通过。10 秒窗口内按键任务累计运行 2706µs、报告为 0.02%，栈最小剩余 2504/3072 字节。

## 固定硬件与构建

- HALLEY7-M-KIT，X2600M，64MiB DDR2，16MiB SPI NOR；CPU 1.152GHz、MPLL 1.8GHz，供电 5V。
- UART0 DEBUG 为 COM52，115200、8N1。SoC UART 为 1.8V，使用开发板原有串口连接。
- 闪迪 32GB TF 已为 FAT32。卡座板 3.3V 输入，已经接到 J9。SWT6621-SH 子板暂不连接，首版不联网。
- 工程使用 Windows CMake/Ninja 和现有 XBurst2 ELF GCC 7.2.0，不依赖 MinGW Make。
- SDK 与 U-Boot 保持各自原有工程；本工程不覆盖 Hello 工程。构建过程不烧录，也不操作真实 TF 卡。

在仓库根目录运行：

```powershell
.\scripts\build-regba.ps1
```

输出目录为 `build/regba-Release`，包含 `halley7m_regba.elf`、`.bin`、`.map` 和 `manifest.json`。脚本会检查入口、ABI、未解析符号、NOR 大小、DDR 堆预算，以及未启用的回放/即时存档缓冲确实被移除。准备给 TF 的资源放在 `build/sdcard`，需人工核对后复制；该目录不是物理 TF 挂载路径。

TF 接线如下：

| 信号 | SoC / 功能 | 接头 / 针号（未注明均为 J9） |
|---|---|---:|
| VDD | 3.3V 标称；用户确认本次 VDD 松动，已接牢，未报告针号新变化 | J10 / 12（原 J9 / 14） |
| GND | 最近确认 J9 pin6；最新重接后待再次确认，pin2 为同一地网 | 6 |
| CLK | PB3 / SSI0 FUNCTION0 | 9 |
| MOSI / DI / CMD | PB2 / SSI0 FUNCTION0 | 5 |
| MISO / DO / DAT0 | PB1 / SSI0 FUNCTION0 | 3 |
| CS | PB0 / GPIO 手动片选 | 7 |

TF 驱动只配置 PB0～PB3，不占用 Wi-Fi 的 MSC1 / PC25～PC30。SPI 初始化为 375kHz，初始化成功后以 18.75MHz 为目标并按卡 CSD 的 `TRAN_SPEED` 限速（本次卡上限 25MHz，实际 18.75MHz；此前为 7.03125 / 3.75MHz）；频率依据 MPLL 1.8GHz / CGU 16 / SSI 内部分频计算，未进行示波器频率测量。

## 软件行为与调试入口

FreeRTOS 主核 port 提供抢占、任务切换、异常处理、OST tick、单调微秒时钟和统一 newlib 堆。内存从应用入口 0x80600000 向上规划；切换到应用的栈、异常向量及中断后，不再调用 U-Boot 服务。

任务运行时间统计已启用，使用现有单调微秒时钟和 64 位累计计数，不重置 OST。`cpu [seconds]` 按指定窗口输出主核忙碌/空闲比例、各任务时间、优先级和栈最小剩余量；默认 10 秒，范围 1～300 秒。采样期间控制台等待，游戏、显示和音频继续运行，报告在窗口结束后打印。ISR 时间计入被中断的任务，包含 Idle；没有独立 ISR 占比。实现、统计口径和本轮验收状态见[运行时间统计与静音测试](../../docs/ReGBA运行时间统计与静音测试.md)。

2026-10-16 统计版 RAM 固件已通过 RTOS 与内置核心重译回归，并完成 `test4.gba` 默认启动流程的带屏静音采样：60.009965 秒 CPU 窗口内忙碌 10.79%、空闲 89.21%，ReGBA 10.73%、audio 0.05%，统计闭合率 100.00%；观测帧率约 59.73fps，LCD 约 30.28Hz，音频真实 PCM/DMA 持续运行，显示及音频欠载/错误均为 0。剩余堆 26416064 字节保持不变。该结果不代表所有关卡或 ROM 的固定占用；原始日志和 JSON 为 `build/regba-cpu-test4-20261016.log/.json`。`scripts/profile-regba.py` 可重复采样，默认 `/gba_game/test4.gba`、60 秒，要求 TF 已挂载、显示已开启、核心/音频已停止；全程保持静音并在结束后停止核心和音频。

ReGBA 使用固定提交 `7f379052dc14366e622a96c7cf7fb6b5a7e918a3` 的共享核心及 MIPS JIT。先全量载入一个原始 `.gba` ROM，最大 32MiB；更换 ROM 前保存同步并释放原 ROM，ZIP 暂不支持。当前 GUI 启动时自动开启显示并进入游戏库；完整游戏帧提交到竖屏双缓冲，无空闲后缓冲时跳过显示提交并继续模拟，不把约 59.727fps 的 GBA 核心锁到约 30Hz 的面板刷新。板载 Start/A/B、串口与蓝牙 GBA 十键位图按位或合并，蓝牙手柄补齐方向、Select、L/R，Y 单独作为 GUI 菜单入口。首轮内嵌上游开放替代 BIOS；自有 BIOS 可放在 `/GBA/BIOS/gba_bios.bin`，严格要求 16384 字节，跳过开机动画直接进入游戏。

ARM/Thumb 翻译器每层栈帧约 10472/10440 字节。派生核心将发射与退出判断的递归深度统一限制为 8，核心任务栈为 240KiB，`status` 提供深度峰值与栈最小剩余量。32MiB ROM 首轮短时运行达到深度 8 时，栈最小剩余 160760/245760 字节；后续无屏 30 分钟及带屏短测的最小剩余稳定为 160312 字节。更多游戏和执行路径仍需覆盖。

FatFs 仅由专用文件任务访问；模拟器的 stdio 通过同步 RPC 转到该任务。调用方返回前保持缓冲区有效；错误使用负 errno，再由 newlib syscall 转换。文件描述符从 3 开始，0/1/2 保留给串口。禁止在 ISR 中调用文件接口。

启动和 `mount` 不创建目录、不执行写入测试、不格式化。显式 `prepare` 创建 `/GBA/{ROMS,SAVES,BIOS,CONFIG}`，保留已有文件。当前代码支持 UTF-8 应用路径与 FatFs UTF-16 转换，文件名单项最多 255 UTF-8 字节、文件层路径最多 512 字节，ROM 入口仍最多 511 字节；边界和错误由主机回归覆盖，8 个中文 ROM 名已从物理 TF 正确读出，中文路径启动、GUI 保存 / 切换 / 退出及存档 / 备份完整读回已实板通过。FAT 时间戳使用固定 2026-01-01，未接 RTC。

原生存档位于 `/GBA/SAVES/<ROM文件名>.sav`，支持 EEPROM / SRAM / Flash 的五种标准文件长度。游戏数据最后一次修改后空闲 2 秒提交独占快照，由后台任务写卡；该任务在首次保存时准备资源目录。`save` 可在运行中请求立即提交，`stop` 和切换 ROM 会等待并刷新待保存修改；切换前失败会保留旧游戏并取消切换。写入经 `.sav.tmp` 检查写完和同步关闭，再保留 `.sav.bak` 上一版本、替换正式文件；缺失正式文件时恢复 `.bak`，坏长度或读取失败则拒绝加载。`.sav` 不包含即时存档状态，最新修改至后台提交完成仍有断电丢失窗口。`status` 中 `backup` / `SAVE` 行区分待保存、后台忙碌、已完成及错误，CRC 为快照内容，需与实际文件读回分别核对；A / B 三类诊断往返和 H 真实存档 TF 回读已通过，新的游戏内保存点恢复仍待验证。

SPI 读响应使用发送 0xFF 的全双工交换，不使用 SDK `HAL_SPI_Receive` 的 0x00 空闲发送值。CS 覆盖完整 SD 事务，交换完成后等待控制器空闲；HAL 超时会恢复 FIFO、锁和 busy 状态。读写数据逐扇区校验 CRC16，采用 16 项半字节查表；命令生成 CRC7。当前单扇区读用 CMD17、多扇区读用 CMD18 并以 CMD12 停止，写入仍以 CMD24 按单扇区循环；每次交换至多 512 字节，派生 HAL 将已排队 / 在途 / 未读字符限制为 64，并使用 1kHz RTOS tick 检查超时，未实现 DMA、热插拔或 SD 卡擦除/格式化。

串口命令：

| 命令 | 用途 |
|---|---|
| `help` | 查看当前固件支持的命令及参数 |
| `status` | 查看运行状态、堆/栈、帧时间及音频统计 |
| `buttons` | 查看 SW6/Start、SW3/A、SW4/B 的板载输入、消抖稳定掩码及按下/释放计数；也包含在 `status` 中 |
| `cpu [seconds]` | 采样任务运行时间，默认 10 秒、范围 1～300 秒；窗口结束输出忙碌/空闲、任务占比、优先级和栈最小剩余量，采样中控制台不处理新命令 |
| `lcd on` / `lcd off` | 核心停止后请求开启或关闭显示；开启时分配 720×1280 XRGB8888 双缓冲，关闭时确认 SRDMA 停止再释放 |
| `lcd test` | 核心停止后显示帧缓冲测试图，含外框、上下方向标记及中央彩条；也用于从 VPG 诊断恢复帧缓冲显示 |
| `lcd diag` | 只读输出 LCD/DSI/SRDMA 状态和帧缓冲像素，用于诊断；寄存器正常不替代可见画面验收 |
| `lcd vpg` | 切换到 DSI 内部彩条诊断；尚未实板验收，使用后先执行 `lcd test` 返回帧缓冲，再运行游戏 |
| `ram-test` | 独占分配应用堆块，经 uncached 地址检查真实 DDR；保留约 2MiB，不覆盖全 64MiB，建议在载入 ROM 前执行 |
| `rtos-test` | 执行 RTOS 底座诊断 |
| `mount` | 识别 TF 并挂载 FAT32，不写入目录和测试文件 |
| `ls /` | 列目录；也可指定 `/GBA/ROMS` |
| `prepare` | 显式创建资源目录 |
| `crc /GBA/ROMS/test.gba` | 读取文件，打印字节数和 CRC32 |
| `rwtest` | 创建新的 `/GBA/TEST/SDTESTxx.BIN`，写入 128KiB、同步、关闭、读回校验；保留文件供重启复核 |
| `jit-test` | 执行 JIT 代码可见性诊断 |
| `core-test` | 内置功能 ROM 先执行 120 帧，清空 JIT 缓存后从同一客体状态再执行 120 帧，比对像素与检查值 |
| `load /GBA/ROMS/game.gba` | 加载 TF ROM；路径可包含空格 |
| `run 120` | 执行指定帧数；`run 0` 持续运行 |
| `stop` | 请求在下一帧边界停止核心，并立即取消测试音输出 |
| `keys 001` | 设置串口十位 GBA 按键掩码；0 仅清除串口输入，板载按住的键仍有效。0x001 为 A，0x002 为 B，0x004 为 Select，0x008 为 Start，0x010/020/040/080 为右/左/上/下，0x100/200 为 R/L |
| `save` | 请求保存原生 `.sav`；运行中在帧边界提交，核心已停止时等待写入完成。以 `SAVE complete result=0` 和 `status` 结果确认实际写卡成功 |
| `audio on` / `audio off` | 启用或停用游戏音频输出；`audio on` 解除静音，`audio off` 也会立即取消测试音 |
| `audio silent` | 启用游戏音频 PCM/转换/DMA，但保持 codec DAC 静音、PE17 功放关闭；用于无声检查 |
| `tone` | 仅按需手动执行：核心空闲时播放 2 秒、500Hz、约 -30dBFS，首尾各 50ms 渐变；当前已加载，尚未进行声音验证 |

内置音频保留双声道 S16 输入，转换到 48kHz，并以 32 位中间值混为单声道。D 起源码的 DMA 环为 6 个 10ms 周期、启动 / 恢复预填充 5 个周期，F 另在首播使用 16 样本真实 FIFO 预填与 464 样本首 DMA；初始化不自动发声。A～C 的预填充为 4 周期，各版实板结果分别记录。屏幕按阶段 5 单独验收，三枚板载用户键按阶段 6 单独验收；当时完整方向键和 Select/L/R 尚未接入，后续蓝牙手柄已补齐，见本文顶部。外置 I2S codec、Wi-Fi、即时存档和 RTC 尚未接入。

阶段 0～4 的第六版及后续显示 RAM 固件均包含短音和取消修订，本轮未播放测试音。`status` 包含 `muted`、非零输入帧数 `nonzero` 及输入峰值 `peak`，用于区分静音输出与无 PCM 输入；显示统计另有 `active`、`vblank`、`presented`、`skipped`、`underrun`、缩放耗时与分配量。静音检查不代表扬声器声音质量通过。

2026-10-17 音频修订版另输出 `audio_timing`：完整 PCM 周期最大发布间隔 `max_ready_gap_us`、最近一次欠载的发布间隔 `underrun_ready_gap_us`、DMA 最大重装耗时 `max_dma_restart_us` 与首次队列欠载时间 `first_queue_underrun_us`。D 的 `first_under_*` 在首次欠载时锁存此前最后一次发布留下的 ready / active / DMA age 快照，描述发布时的状态；多次欠载后不要把它与最近一次的 `underrun_ready_gap_us` 合为同一事件。这些是软件观察点统计，不能当作硬件 FIFO 变空时刻的测量；本轮默认仍保持静音。

## 阶段验收

| 阶段 | 软件交付 | 实板必须取得的证据 |
|---|---|---|
| 0：构建与启动 | 可追溯 ELF/BIN/MAP、启动入口和串口诊断 | 完整 16MiB NOR 备份及校验；DDR 容量/边界验证；正常启动日志；连续 10 次冷启动 |
| 1：FreeRTOS | 主核 port、统一堆、异常和时间基准 | 抢占/阻塞/ISR 通知/临界区、HI/LO/FPU 保存恢复；时钟单调与低 32 位回绕；压力运行 30 分钟无异常和栈越界 |
| 2：TF/FatFs | SD SPI 协议、文件任务、stdio 桥、文件测试 | 20 次冷启动挂载；32MiB 文件与电脑 CRC 一致；128KiB 文件写入同步、重启读回；无卡和超时返回错误 |
| 3：ReGBA/JIT | 核心编译、代码缓存发布、无屏帧/音频输出 | 同地址重新生成代码后执行更新；内置测试 ROM；ARM/Thumb、IRQ/SWI、RAM 执行、自修改代码回归；JIT 清空重译结果一致 |
| 4：内置音频 | 重算时钟、DMA 队列、速率转换和统计 | 单音音高/时长正确；连续 30 分钟无欠载；暂停恢复正常；并行文件写入不增加欠载 |
| 5：竖屏 MIPI | ZC502 初始化、XRGB8888 双缓冲、3 倍缩放、显示状态和诊断 | 静态图、真实游戏、320 秒联合短测及第四版开关回收/60 秒回归通过；后续 H 带屏 30 分钟联合压力通过，用户确认画面正常。断电冷启动重复性和更多游戏仍待覆盖 |
| 6：板载 Start/A/B | PE13/PE14/PE15 输入、扫描消抖、组合键、串口输入合并和状态诊断 | Release/Debug 构建及主机消抖回归通过；用户确认 `test4.gba` 三键响应，松开回零；物理长按/组合及输入合并待逐项实板验证。方向、Select、L/R 属后续输入扩展 |
| 7：原生存档与音频修订 | 2 秒空闲自动保存、独占快照后台写卡、停止 / 切 ROM flush、上一版本恢复、坏档拒绝；音频竞争修复和时间统计 | 三类型诊断往返 / 跨复位恢复通过；H 真实非零 PCM 静音生命周期、运行态完整 ROM CRC、手动原生保存 / TF 读回及带屏 30 分钟联合压力通过，全部 SAVE 样本无错误或待提交修改。新的游戏内保存点恢复及听感仍待专项确认 |

默认复核顺序：先启动和 `ram-test` / `rtos-test`，然后 `mount`/`ls`/只读 `crc`，再运行 JIT 和内置核心测试。显式写入验证使用 `prepare`、`rwtest`；重启后对日志中保留的测试文件运行 `crc`，确认字节数和 CRC 一致。游戏音频先用 `audio silent` 检查 PCM/DMA 和统计；需要实际听音时提前告知，仅按需手动执行一次短 `tone`，不纳入默认命令序列。

`rwtest` 使用 `FA_CREATE_NEW` 依次寻找 00～99 中不存在的测试文件，不覆盖、不删除已有测试文件。卡满、目录名冲突和 SD 错误会返回失败；没有自动格式化或原始扇区破坏性测试。

NOR 首次写入前必须完成备份并具备恢复方法。现有布局为 U-Boot 0x0，环境区起点 0x3F000，应用起点 0x40000、分区 6MiB、入口 0x80600000；不得把整卡或整个 NOR 当作读写测试介质。

本项目提供 `scripts/backup-nor.py`，采用已固定 SHA256 的官方 RAM SPL/helper 和本板 DDR2 参数。默认只验证本地输入并打印方案，不打开 USB：

```powershell
.\.venv\Scripts\python.exe scripts\backup-nor.py
.\.venv\Scripts\python.exe scripts\backup-nor.py --self-test
```

2026-10-16 已在实板执行只读备份：USB Boot `A108:EAEF`，CPU `X2600`，NOR JEDEC `0x852018`。完整 16MiB 读取两遍，每块设备传输 CRC 均通过，两遍落盘长度和 SHA256 一致，元数据状态为 `verified`。证据位于 `build/nor-backups/20261016T024011Z/`，包含 `nor-original.bin`、`nor-pass2.bin` 和 `metadata.json`；两份镜像的 SHA256 均为 `5efd956157cf88fc8a55dd3695f75b216e184a9fa15bee00149680c740077926`。备份过程未发送 NOR 擦写请求。

后续重新备份时，手动进入 USB Boot、关闭 Cloner 等占用工具，显式 `--execute` 才启动读取。脚本保留输入版本、JEDEC、读地址、长度和请求集合；任何失败均保留失败状态和已有数据，不将其认作可恢复备份。备份结束后按下一项调试需要复位为正常启动或重新进入 USB Boot。

```powershell
# 仅在已进入 USB Boot 且没有其他工具占用设备时执行。
.\.venv\Scripts\python.exe scripts\backup-nor.py --execute
```

协议依据为本地官方 Cloner 2.5.54 源码：`arch/mips/lib/board.c` 在 RAM helper 启动时建立默认标准读表；`f_jz_cloner.c` 的 `VR_READ` 直接调度 `sfcnor_read`，使用默认 0x03/24 位地址读取；`VR_GET_CRC` 排队 USB IN，再取数据和 ACK。RAM 配置仅含 POLI 和 DEBUG，不传 SFC 编程参数。请求白名单不包含 `VR_INIT`、`VR_WRITE`，不会进入含状态寄存器修改及擦除逻辑的 `norflash_get_params_from_burner`。上述实板双遍一致备份已满足阶段 0 的备份要求；新固件 RAM 启动已另行验证，其余验收项按对应阶段记录。

## 已运行的验证与未完成项

阶段 2 的五个 C 源已使用本地 MIPS GCC 7.2.0 通过语法检查及目标文件编译。以下脚本自动通过 vswhere 定位 MSVC（或使用已安装的 VS18），加载 vcvars64 环境，并构建、运行存储、音频、显示像素转换、按键消抖及原生存档测试：

```powershell
.\scripts\test-regba-host.ps1
```

测试产物输出在 `build/regba-host`。音频测试每次从生产 `audio.c` 提取 `audio_submit_stereo` 及 start/stop/queue 片段，注入固定测试模板，不使用旧的复制实现。脚本不访问 USB、物理 TF 或 NOR；存档故障测试在 `build/regba-host/battery` 创建并清理自己的本地临时文件。原生 MSVC 测试真实运行通过以下场景：

- SD CMD0/CMD8 的 CRC7、CRC16 标准向量、CSD v1/v2 容量和溢出、CP437 双向转换。
- 64MiB 纯 RAM FAT32 卷及 MBR 分区：长文件名/大小写查找、128KiB 写入/同步/重挂载逐字节读回、EOF/seek、重命名和删除。
- 音频 140 组采样率/提交块大小/队列背压组合，逐采样比对独立绝对时间参考计算；空指针、非法速率和中途更换速率检查。
- 音频生产 start/stop/queue 代码的背压、部分/完整周期提交时停止交错、初始化/稳定等待期间取消、静音保持和周期发布时间统计。
- 显示直接编译生产 `platform/video_blit.c`，以 17 组固定 RGB/灰度预期检查位序、5 位到 8 位扩展和 bit15 忽略；源 stride=256、目标 stride=736，两轮翻转 bit15，覆盖全部 3× 区域、居中位置、上下边框、每行 padding 及前后哨兵。每轮检查 942208 个目标字与 41088 个源字；源内容保持不变。完整入口通过的日志为 `build/regba-host-display-20261016.log`。
- 按键直接编译生产 `platform/buttons.c` 的消抖状态机，覆盖短毛刺、19ms/20ms 边界、长按保持、释放抖动、组合键、各键独立消抖、32 位计时回绕与无关位过滤；不访问 GPIO 或 FreeRTOS 调度。
- 原生存档直接包含生产 writer/loader，检查五种大小、首次与版本替换、短写/短读、open/flush/close 错误、删除旧备份与 rename/rollback 失败、缺档恢复、忽略临时文件、坏长度及容量不足。2026-10-17 完整回归日志为 `build/regba-save-audio-host-20261017.log`。

存储协议和算法测试使用主机 RAM，存档测试另访问构建目录中的本地测试文件，均不访问物理 TF。`storage_host_test.c` 不能加入固件目标。可在 VS Developer Command Prompt 中运行：

```bat
cl /nologo /std:c11 /utf-8 /DSDSPI_HOST_TEST /Iprojects\halley7m-regba\platform projects\halley7m-regba\platform\storage_host_test.c projects\halley7m-regba\platform\sdspi.c projects\halley7m-regba\platform\fatfs\ff.c projects\halley7m-regba\platform\fatfs\unicode.c /Fobuild\storage-host\ /Febuild\storage-host\storage_host_test.exe
build\storage-host\storage_host_test.exe
```

这些原生测试不覆盖 SSI 引脚、电气、真实 SD 卡时序、RTOS RPC 调度、newlib 桥、JIT 实板执行、音频 DMA、显示 DMA/缓存/PHY 或实际屏幕颜色。Release/Debug 主工程均已成功链接；实际构建大小、哈希和堆边界见对应 `manifest.json`。阶段 0～4 的详细实板结果见[实施记录](../../docs/ReGBA阶段0-4实施记录.md)，显示结果见[阶段 5 记录](../../docs/ReGBA阶段5显示实施记录.md)。

2026-10-16 首轮 RAM 调试暴露任务栈溢出、FCSR 自测期望和音频寄存器检查问题，历史日志为 `build/regba-bringup-20261016.log`。后续修订的实板证据如下，完整过程见 `docs/ReGBA阶段0-4实施记录.md`：

- 第二版日志 `build/regba-bringup-20261016-second.log`：32MiB `/gba_game/test1.gba` 完成 120 帧，耗时 2013521µs、画面 CRC32 `1ce3f149`；翻译深度峰值 8，栈最小剩余 160760/245760 字节。随后连续运行 541 帧并接受串口 Start/释放/stop，未再溢出。尚未接屏，不能据此认定游戏画面或兼容性验收完成。
- 实际 TF/FAT32 挂载成功，62333952 个扇区、每簇 64 扇区。重启后读取首轮 `/GBA/TEST/SDTEST00.BIN`，131072 字节、CRC32 `29df376d`，与写入结果一致。32MiB ROM 板端与电脑端 `E:/gba/test1.gba` 均为 33554432 字节、CRC32 `fc7b376f`，对比通过；证据见 `build/rom-integrity/20261016-test1.json`。20 次冷启动未完成。
- 第三版日志 `build/regba-bringup-20261016-third.log`：FCSR 探针改为写入后原子记录完整硬件读回值，保留对整个值的跨抢占比较，解决固定读回位 `0x000c0000` 造成的误报。RTOS 自测通过 32/32 轮、抢占 102/101 次、OOM 后心跳 49。
- 第四版日志 `build/regba-bringup-20261016-fourth.log`：修正自动 D 计算读回 `0xbb` 与 MDIV 奇数取偶读回 0 的检查误判后，实际测试音稳定连续，用户已确认。历史 60 秒播放完成 6000 个 DMA 周期，FIFO 欠载、DMA 错误和超时均 0；队列欠载 1 出现在结尾数据耗尽。另一次播放期间 `rwtest` 新建 SDTEST02，DMA 完成数 274→884，播放中队列/FIFO 欠载、错误和超时均 0。此为首轮音频及并行写卡证据，未完成 30 分钟或游戏音频验收。
- 第四版再次通过 `jit-test`、内置 `core-test` 120 帧（2013542µs、CRC32 `19b3e686`、mailbox 全部正确、VBlank IRQ 118）与 `rtos-test`（32/32 轮、抢占 102/101 次、OOM 后心跳 49）。

第四版 RAM 载入日志为 `build/ramload-20261016-113816-f74b3c.log`；BIN 707952 字节、SHA256 `b10b396650921ad92b32fb382ba85b32378db594eb4eeedf4f08fac6f5456485`、板端 CRC32 `ef047501`，传输和校验后跳转，未写 NOR。用户反馈后新增短音/停止修订，并静态修正已排队测试音的取消竞态，仅在接收新命令时清取消标志；该修订已随第五版载入 RAM，未重复播放测试音。

第五版载入日志为 `build/ramload-20261016-120754-7b8b49.log`，BIN 711264 字节、SHA256 `881fe00866977d522871a1b008a644daf6c385cbf9eba3ce79c7ad3128493549`、板端 CRC32 `c84cada3`；长度与 CRC 校验后从 RAM 启动，未写 NOR。Release/Debug 均编译及产物检查通过：Release 静态布局后堆 48500736 字节，Debug BIN 1325648 字节、堆 47886336 字节。第五版命令日志为 `build/regba-bringup-20261016-fifth.log`：

- `ram-test` 检查 cached `[0x81206580,0x83dfff80)` / uncached `[0xa1206580,0xa3dfff80)` 共 46111232 字节，六种模式与 64 项 walking 检查通过，耗时 14096342µs。仅覆盖其独占分配的应用堆，不覆盖保留内存、正在使用的代码/数据/任务栈或全 64MiB。
- `rtos-test` 再次通过 32/32 轮、抢占 102/101 次、OOM 后心跳 49、ISR 栈剩余 16252 字节；`jit-test` 同地址重写 `37 -> 73` 跨 tick 通过。
- `core-test` 清缓存前后各 120 帧，耗时 2012892µs / 2013565µs，RGB555 CRC32 均为 `19b3e686`；VBlank IRQ 118→238，`core-retranslate-test PASS`。清空 READONLY/FULL_CACHE 时级联清空可写缓存，第二轮延续同一客体状态，不等同于重新执行全部初始化路径。
- TF 重启后再次读取 SDTEST00，131072 字节、CRC32 `29df376d`，与最初写入结果相同。
- `audio silent` 下，32MiB ROM 连续运行 1379 帧、23095366µs；翻译深度峰值 8，栈最小剩余 160336/245760 字节。Start/释放后，第 1260 帧非零 PCM 输入帧数 759312、输入峰值 23952、DMA 完成数 2102，保持 `muted=1`。但 `queue_under=2 FIFO_under=3`，错误和超时为 0；已停止并进入修正，不能据此认定游戏音频或 30 分钟压力通过。

欠载定位到旧 console 优先级 4 高于核心任务 3，115200 波特率下的 `status` 忙等输出约占 48～56ms，超过旧 4×10ms 队列。最新源码将 console 改为与核心相同的优先级 3，并将队列增至 6×10ms、启动预填充增至 4×10ms；重译自测也加入第二轮 IRQ 必须增长的断言。修订版 Release BIN 711376 字节、堆 48500736 字节，Debug BIN 1325792 字节、堆 47886336 字节；均编译及产物检查通过，已随第六版加载到 RAM；一分钟及 30 分钟游戏复测均未再出现队列/FIFO 欠载。

第六版加载日志为 `build/ramload-20261016-122140-6762a0.log`，Release SHA256 `9509d0d3e7fecc827375a5ba06d5e3674a12b14096e7a809560fcfdf1f9a2acb`、板端 CRC32 `ecbdff41`，未写 NOR。命令日志 `build/regba-bringup-20261016-sixth.log` 记录 RTOS 再次通过 32/32 轮、抢占 102/101 次、ISR 栈剩余 16252 字节；清缓存重译两轮各 120 帧耗时 2012942µs / 2013502µs，CRC32 均为 `19b3e686`、IRQ 118→238，增长断言通过；TF 挂载成功。

第六版短测日志 `build/regba-soak-20261016-sixth-short.log` 实际通过 60.062 秒稳态静音游戏检查，不含热身。结束前 3881 帧，音频输入 2863616 / 输出 3116860 帧、DMA 完成 6490、非零输入 2610488、输入峰值 32768；队列/FIFO 欠载、错误和超时均为 0。清理后 3883 帧、DMA 6495，核心/音频均停止、保持静音，四项计数仍为 0。

第六版 30 分钟静音压力日志 `build/regba-soak-20261016-sixth-30min.log` 最终打印 `SOAK 30min PASS`，主机执行退出码 0；稳态主机计时 1800.094 秒、板端 1800.060 秒，不含热身。结束前 107647 帧，PCM 输入 79480320 / 输出 86509191 帧、DMA 完成 180224、非零输入 77286420、峰值 32768；每分钟采样的队列/FIFO 欠载、错误和超时全部为 0。第 5、10、15、20、25 分钟新建 SDTEST03～SDTEST07，五次写入/同步/读回各 131072 字节、CRC32 `29df376d`；第 10、20 分钟读取既有 SDTEST00 的字节数及 CRC 同样一致。

31 个运行样本离线复核：首末帧增量 107513，约 59.727 帧/秒；DMA 约 100 周期/秒，板端有效计时 1800.059960 秒与主机相差约 34.04ms。采样跨过 OST 两次低 32 位计数回绕边界 715.827883 / 1431.655766 秒，64 位时间与 RTOS tick 继续单调；全部样本堆 14644280 字节、核心栈最小剩余 160312/245760 字节保持不变。发送 `stop` / `audio off` 后 107650 帧正常停止、DMA 完成 180229、队列 0，核心和音频均为 `running=0`、音频保持 `muted=1`，四项计数仍为 0。该结果覆盖此 32MiB ROM 的无屏运行及静音音频/TF 并行压力；可见画面随后在阶段 5 另行验证，可闻声音质量、实体按键和广泛游戏兼容性仍待验收。

`scripts/soak-regba.py` 可在已启动、已挂载的固件上进行无声游戏/PCM/DMA/TF 压力。默认载入 `/gba_game/test1.gba`，最长等待 600 秒；`--skip-load` 可复用已经载入且停止的 ROM。脚本确认 `muted=1` 后运行，每 60 秒检查状态、每 300 秒写读新 128KiB 文件、每 600 秒复核 SDTEST00；所有运行中队列/FIFO 欠载、错误和超时必须为 0，时钟、帧数、PCM 和 DMA 必须持续增长。最新脚本还比较 OST 微秒与 RTOS tick 增量，结尾比较板端时间和主机单调时间。结束或失败均停机并关闭音频。`--press-start` 模拟 Start/释放，帮助进入产生非零 PCM 的状态；计时不包含热身，未完成实际运行不能标记通过。

```powershell
.\.venv\Scripts\python.exe scripts\soak-regba.py --duration 1800 --press-start
```

带屏复核须先在核心停止时执行 `lcd test` 或 `lcd on`，并确认 TF 已挂载，再使用 `--require-display`。该选项要求显示已开启、双缓冲分配量正确、显示计数持续增长且 LCD 欠载为 0，不会自行开启显示。脚本结束仍保留 LCD 最后画面；需要关闭显示时再在串口执行 `lcd off`。

```powershell
# 320 秒会覆盖第 300 秒的新建 TF 文件写入/同步/读回检查。
.\.venv\Scripts\python.exe scripts\soak-regba.py --duration 320 --press-start --require-display
```

以上 2026-10-16 历史版本结束时，`.sav` 往返和带屏长压尚未执行；后续 A / B 已完成三类诊断往返及普通复位恢复，H 已完成真实手动保存 / TF 回读和带屏 30 分钟联合压力，详见[存档与音频实施记录](../../docs/ReGBA存档与音频实施记录.md)。该历史阶段结束时仍待验收 DDR 全容量 / 边界、连续断电冷启动、游戏可闻音质、更多游戏兼容性，以及真实游戏新保存点的重载恢复。短音与取消行为未做声音验证。阶段 6 已确认三枚键基本响应，物理长按 / 组合及两路输入合并仍待逐项验证；当时完整方向、Select、L/R 尚未接入、未固化到 NOR；后续 UART2 手柄和当前首次 NOR 安装结果见本文顶部。

stdio 到文件任务的阻塞链已完成静态核对：malloc/sbrk 的调度暂停在返回前配平，分配器不调用文件 RPC；实际链接的 newlib `__sfp_lock_*` 和 `__sinit_lock_*` 反汇编均直接返回。文件 RPC 只在调度器运行态阻塞，暂停态会返回错误；最新源码中 console、文件任务和核心任务均为优先级 3，启用时间片。文件任务打印通过 fd1 直接写 UART，不递归调用文件 RPC。第五版发现的高优先级 console 忙等导致音频生产者受阻已修订，第六版 30 分钟实板调度与并行文件读写未复现欠载或死锁。

### MIPS ISA 模拟器核心验证

`tests/regba_sim.c` 将生成后的真实 ReGBA 核心、MIPS JIT 和 ABI 修补后的 `stub.S` 链接为独立程序，不包含 FreeRTOS 或板级驱动。使用默认内置替代 BIOS 和自主诊断 ROM，运行 120 帧后检查 ARM/Thumb 切换、BIOS/HLE 除法、RAM 代码执行、自修改代码、VBlank IRQ 和全部 38400 个 RGB555 像素。

早期生成核心已在 WSL Ubuntu-22.04 的官方 Linux 版 MIPS simulator 上实际通过：六个 mailbox 值为 `42,55,66,77,6,0x600D`，VBlank IRQ 计数为 118，全屏像素为 `0x3DEF`，程序退出码为 0。首次运行的控制台分段输出已合并保存在 `build/regba-sim/simulator.log`；其中 events/PC 为当时临时诊断输出，当前 harness 只保留帧进度。

新增 `--depth-regression` 在诊断 ROM 入口前追加 32 个 ARM 分支块，以当前生成核心实测翻译深度峰值为 8、结束时深度为 0，随后仍通过上述 120 帧全部检查。日志为 `build/regba-depth-sim/simulator.log`，实际退出码 0、耗时 24.092 秒。此项为深度限制的软件回归，与实板缓存和 RTOS 结果分别记录。

先构建固件以生成 `build/regba-Release/regba-core`，然后在 WSL 内运行以下可复现脚本。它从当前生成核心重新编译，并保存 simulator 输出，最长运行时间默认 45 秒：

```bash
cd /mnt/e/GitProject/X2600
python3 scripts/test-regba-sim.py --timeout 45
python3 scripts/test-regba-sim.py --depth-regression --output build/regba-depth-sim --timeout 45
```

也可在 Windows 中使用 `python scripts/test-regba-sim.py --build-only` 生成相同用途的 ELF，再交由 WSL Linux simulator 执行。本地 Windows 版 `mips-sde-elf-run.exe` 在最小 hello 的首次间接跳转处发生宿主 `0xC0000005`，因此不用于判定核心正确性；Linux 版已通过相同 hello 和核心测试。

该验证仅覆盖软件核心和 ISA 执行。模拟器的指令/数据内存视为一致，`ReGBA_MakeCodeVisible` 为测试空实现；不覆盖实板缓存发布、FreeRTOS 上下文切换、TF 电气和时序、文件任务调度、音频 DMA 或实际游戏兼容性。

### 阶段 5：屏幕核实、游戏画面与联合短测

原 NOR 备份偏移 `0x40000` 的 Linux 5.10.186 uImage 数据 CRC32 为 `561bdeb6`；解压内核 13145632 字节，内核偏移 `0xC78260` 的 DTB 为 28371 字节、CRC32 `e7b63d61`。DTB 板型 `ingenic,x2600-halley7`，DPU 为 `okay`、旋转 0、前两层 720×1280 各两个帧缓冲，并有 lane2/3 PN swap。FW050 与 `zc50289hshd02` 节点均为 `okay`，但 DPU 的 `remote-endpoint=0x2b` 实际连向 ZC502，FW050 endpoint 没有 phandle。随后启动原 Linux 核实 `/sys/bus/platform/drivers/zc50289hshd02` 实际绑定 `display-dbi:panel_zc50289hshd02`，FW050 无绑定设备；fb0 模式为 `U:720x1280p-30`、32bpp、stride 2880、虚拟大小 720×2560。

panel 电源/复位 PC11/PC10、背光 PWM15_PC14 与 `u-boot-ingenic/include/configs/x2600m_halley7.h` 一致。随包 MIPI V1 图的 PC08/PC15/PE02 属另一版本，不能套用；`libbare-cpu/projects/x2660-halley/Examples/lcd-mipi-fw050/fw050.c` 的开关电 GPIO 也存在 PC11/PC10 与 PC08/PC15 不一致。实际配置为 2 lane、RGB888 burst、LCD 37.5MHz；PHY 时序及模拟寄存器来自正常显示的 Linux，202 条 ZC502 初始化命令与原 NOR 内核逐条比对一致。证据为 `build/linux-display-probe-20261016.log` 和 `build/linux-display-registers-20261016.log`。

显示代码在本工程内增加 `platform/panel_zc502.c`、`video.c`、`video_blit.c`；`scripts/prepare-display.py` 只在构建目录派生三个 SDK 源文件及哈希记录，不修改 SDK。两个 XRGB8888 帧缓冲共 7372800 字节，按 64 字节对齐动态分配。显示 ISR 只清中断及计数；核心在提交帧内容和 RDMA 描述符前发布缓存，后缓冲尚未空闲时跳过此次显示提交。

前两版 RAM 显示固件背光亮但黑屏，日志分别为 `build/regba-display-pattern-20261016.log` 和 `build/regba-display-second-20261016.log`。第三版在复位释放后增加 120ms 等待，再发送初始化表；用户确认彩条全部正常、无异常。载入日志为 `build/ramload-display-third-20261016.log`，Release BIN 731840 字节、SHA256 `0e3b79d696b7508b77ed0b41367f838b0da0a1404a0b9d2a44cc39904c0d8d73`、RAM CRC32 `dfd54f2d`；未写 NOR。

第三版显示开启执行 `core-test` 两轮各 120 帧，耗时 2012980µs / 2013287µs，RGB555 CRC32 均为 `19b3e686`，VBlank IRQ 118→238、`core-retranslate-test PASS`；随后 LCD `underrun=0`、末次缩放/缓存发布 977µs，日志为 `build/regba-display-coretest-20261016.log`。此后加载已校验的 32MiB `/gba_game/test1.gba`，用户确认真实游戏画面正常、没有异常。

同一第三版完成显示、真实 ROM、静音游戏 PCM/DMA 与 TF 的 320 秒联合短测。日志 `build/regba-display-soak-20261016-320s.log` 打印 `short-run PASS`，实际主机计时 320.133 秒、板端稳态 320.125346 秒；19121 个客体帧、平均 59.72973fps，LCD 及显示提交均为 30.27252Hz，DMA 约 100Hz。七个运行样本中空闲堆均为 7251960 字节，核心栈最小剩余 160312 字节，LCD 欠载以及音频队列/FIFO 欠载、错误、超时全部为 0。第 300 秒新建 `/GBA/TEST/SDTEST08.BIN`，131072 字节、CRC32 `29df376d`，写入、同步、读回通过。精确汇总在 `build/regba-display-soak-summary-20261016.json`。此项为第三版带显示短测，与先前第六版无屏 30 分钟压力分别记录。

第三版显示关闭后重开曾出现 PHY stop-state 警告。最终第四版在开启前及关闭后清除 `LPCLK_CTRL`、`PWR_UP`、`PHY_RSTZ`，复位 DSI 主机和 PHY 后再初始化。Release BIN 731840 字节、初始堆 48480256 字节，SHA256 `8f6b26a5c3f2412d2155cf8427f8544e6690f5f297a179a628ead07ea2df1152`，RAM CRC32 `1cdc106f`；Debug BIN 1366176 字节、初始堆 47845376 字节。Release/Debug 编译与产物检查通过，载入日志为 `build/ramload-display-fourth-20261016.log`，未写 NOR。

第四版的首次显示开启及两轮 `lcd off` / `lcd test` 均成功、没有 PHY 警告，用户确认重开后的彩条正常。开启空闲堆 40814176 字节，关闭后恢复到 48187048 字节，每轮完整回收 7372872 字节，包含双缓冲图像和分配对齐开销；LCD `underrun=0`。日志为 `build/regba-display-fourth-lifecycle-20261016.log`。这是同一 RAM 启动内的开关重复验证，不计作断电冷启动。

第四版保持显示开启再载入同一 32MiB ROM，加载后空闲堆 7257240 字节，并执行 `--require-display` 的 60 秒静音回归。日志 `build/regba-display-fourth-regression-20261016-60s.log` 为 `short-run PASS`，主机计时 60.161 秒，板端 60.159024 秒，稳态帧数 293→3886、约 59.7250fps；运行中空闲堆 7251960 字节、核心栈最小剩余 160312 字节，LCD 和四项音频错误计数全部为 0。精确统计见 `build/regba-display-fourth-summary-20261016.json`。清理后核心停在 3890 帧，音频 `running=0 muted=1 queued=0`；LCD `active=1 vblank=7165 presented=1973 skipped=1918 underrun=0`，保留最后画面。COM52 已释放，未写 NOR。

以上为 2026-10-16 阶段 5 第四版的验收范围：静态图、此 ROM 的可见游戏画面、带显示联合短测及关闭回收 / 重开通过，当时尚未做带屏长压。完整历史见[阶段 5 显示实施记录](../../docs/ReGBA阶段5显示实施记录.md)。后续 H 已完成本游戏带屏 30 分钟压力，原生存档的三类型诊断往返 / 跨复位恢复与真实文件读回也已取得证据，见[存档与音频实施记录](../../docs/ReGBA存档与音频实施记录.md)；DDR 全域、重复冷启动、更多游戏 / 完整实体输入、真实游戏新保存点恢复和音质仍保留各自验收边界。

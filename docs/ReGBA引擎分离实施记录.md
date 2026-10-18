# ReGBA 引擎接口抽取与适配层分离

本轮先完成 ReGBA 的接口和职责拆分，通过回归后再接入 mGBA。继续使用现有仓库，每个阶段完成相关验证后，用中文标题和正文分别提交并推送。当前文档首先固定阶段 0 的比较基线，后续阶段按实际结果补充。

## 阶段 0：固定比较基线

日期：2026-10-18。源码基线为 `144218b5645997f8ecf0d82d6cc20e6a8cb1d2bb`，即“文档：约定阶段完成后使用中文提交并推送”。以下产物和实板结果属于拆分前版本，不能作为拆分后的验收结论。

固定依赖如下：

| 依赖 | 提交 |
|---|---|
| SDK / FreeRTOS，`libbare-cpu` | `9500d2ec917a44d0d9657940e865e70f1a885849` |
| ReGBA | `7f379052dc14366e622a96c7cf7fb6b5a7e918a3` |
| 可选 U-Boot | `501177955aab5f844d52a0a8ea3802e704f9ceca` |

比较对象是 GUI 游戏设置版，完整数据见[拆分基线摘要](validation/engine-separation-baseline-20261018.json)与[原版验收摘要](validation/gui-options-20261018-validation.json)。本机 Release / Debug BIN 的实际字节数和 SHA256 均与原版 manifest 一致：

| 产物 | 字节数 | SHA256 | 首任务分配前堆空间 |
|---|---:|---|---:|
| Release | 2306352 | `7517d55258594d485a86b4d7eecb1a8f32abf34674fcccf9727396a5f97e16eb` | 46764032 |
| Debug | 2961200 | `b722c10696029c53ba4f34e6a183fa785ffcb2f8b652dd4cd4045d07632bbe48` | 46108672 |

Release CRC32 为 `178853ea`，入口 `0x80600000`。两个 manifest 的 `hardware_validated=false` 是产物检查生成器的原值；实板结果记录在独立验收摘要中，不能据此推导为 Debug 已经板测。

### 已有验证及其范围

发布前完整主机回归日志为本机 [`build/git-publication-host-tests-utf8-20261018.log`](../build/git-publication-host-tests-utf8-20261018.log)，末尾为 `ReGBA host checks passed.`。测试涵盖路径编码与真实 FatFs 内存卡、重采样、音频启停和首次 DMA、画面缩放与 HUD、任务统计、按键、蓝牙协议与输入、资源路径、GUI、存档原子替换和缓存 ROM 读档、播放控制、SSI/SD 寄存器或卡模型，以及 NOR 安装离线检查。模型中的预期 RED 反例是测试的一部分，不是生产路径失败。本次阶段 0 读取并归档既有结果，没有重新运行该套回归，也没有操作板子。

拆分前 GUI 游戏设置版已经具备以下证据：

- Release / Debug 构建及产物检查、完整主机回归通过。
- RAM GUI 检查覆盖七项菜单、音量 0～100、跳帧 0～3、1× / 2× / 4×和回到 1× 后恢复音频；用户反馈“菜单、音量、加速和声音恢复均正常”。
- SRAM 32KiB、Flash 64KiB、EEPROM 512B 均完成“保存 A → 产生未保存 B → 菜单读取恢复 A”，并核对客体状态与卡上文件，缓存 ROM 不重新读取。
- 32MiB ROM 的 128KiB Flash 存档与缓存读档通过：加载约 28.258 秒、读档约 0.323 秒，运行空闲堆 5522040 字节，作为重构前内存比较窗口。
- 原版 Release 已安装 NOR，经程序回读、环境和保护区核对及软件复位自动进入游戏库检查通过。**本轮重构先在 RAM 验收，不执行新的 NOR 写入。**

原版 GUI 游戏设置版尚未完成新的冷启动、30 分钟联合长测或商业游戏具体剧情保存点恢复。较早 H 版确实完成静音真实 PCM / LCD / TF 的 30 分钟联合测试，但它是不同镜像，只保留为[历史稳定性证据](validation/regba-H-soak-summary-20261018.json)，不能替代本轮阶段 4 的新镜像测试。

### 拆分前职责与外部入口

拆分前的 `core/regba_core.c`（阶段 2 已迁至 [`regba_adapter.c`](../projects/halley7m-regba/engines/regba/regba_adapter.c)）同时管理 FreeRTOS 任务和队列、GUI 会话、输入与播放策略、ReGBA 回调 / JIT、ROM / BIOS、音频样本以及原生存档状态。公共驱动已经分别位于 [`platform`](../projects/halley7m-regba/platform/)；GUI 和播放规则分别有 `handheld_ui` 与 `playback` 模块。

拆分前 `regba_core.h` 的入口包括任务启动、载入、运行、核心自测、保存、菜单、短音、显示控制、停止、串口键位、音频请求及状态查询。阶段 1 先经新接口转发，阶段 3 移除旧头，统一使用 [`emu_engine.h`](../projects/halley7m-regba/core/emu_engine.h)；串口命令、GUI 菜单、存档路径和错误含义不因重命名而改变。

### 必须保留的行为契约

1. **菜单暂停与保存分开。** 打开菜单只等待已经提交给后台的快照完成，即 `settle_backup`；不新提交当前 dirty 内容。这样才能从卡上读取旧进度。显式保存、普通停止、换游戏和退出使用 `flush_backup`。保存失败保留当前 ROM 和内存中的未保存内容，并停止切换。
2. **ROM 所有权属于引擎适配层。** 当前 ROM 为完整映射的原始 `.gba`，最大 32MiB。适配层统一持有 ROM 缓冲和对应文件，卸载先完成保存同步，再关闭文件并释放。公共会话不接管 ReGBA 裸指针。读原生存档时复用现有 ROM，失败保留 guest、存档类型和 dirty 状态。
3. **常驻音频资源先于首次大 ROM 分配。** GUI 初始化音频任务 / 栈的时序不能移到游戏开始之后；此前该分配顺序曾导致连续 32MiB ROM 空间不足。初始化保持无声。
4. **引擎执行模型先保留。** ReGBA 使用 `run` 加帧边界回调；`setjmp/longjmp`、MIPS JIT 和核心全局状态留在适配层。公共层不要求每帧退出 JIT，再重新进入核心。
5. **共享数据交接写清楚。** 接口必须明确视频像素格式、stride 单位、PCM 采样率和声道、缓冲有效期及音频部分消费返回值。保留输入释放门控、每帧服务、跳帧相位、倍速静音和 1× 恢复时序。
6. **验证生产实现。** 当前部分主机测试提取 `regba_core.c` 中的生产函数，搬迁后同步改变提取来源和测试桩，保留断言；字库生成输入同步迁移，避免继续测试旧副本或遗漏中文文案。
7. **板测保持静音并先用 RAM。** 不播放测试音或自动解除静音；旧 NOR 保持可用。调试串口由一个进程独占，加载和测试日志保留镜像身份。

## 阶段顺序与放行条件

| 阶段 | 交付 | 放行条件 | 当前状态 |
|---|---|---|---|
| 0：基线 | 本记录、可追溯产物和比较摘要 | 核对源码、依赖、既有验证及产物身份 | 已核对；基线产物检查通过 |
| 1：接口门面 | 公共引擎接口，ReGBA 实现，旧入口兼容转发 | 相关主机检查、双构建和产物校验通过 | 软件验收通过 |
| 2：ReGBA 适配层 | 回调、JIT、ROM / BIOS、私有状态、存档转换分离 | 公共头不暴露 ReGBA 类型；相关生产实现回归通过 | 目录迁移已通过；职责分离随阶段 3 完成 |
| 3：公共会话 | 任务、队列、GUI 调度、输入与播放策略 | 会话不读取 ReGBA 全局；保存、读档、切换和退出顺序正确 | 软件验收通过；进入阶段 4 板测 |
| 4：ReGBA 回归门禁 | 新镜像与完整软件、RAM 实板验收记录 | 主机回归、双构建、核心诊断、32MiB 切换和存档、GUI、静音音频启停、30 分钟带屏 / PCM / TF 联合测试 | 已通过，人工画面观察正常 |
| 5：mGBA 独立构建 | 固定上游版本、最小 GBA 核心和平台适配 | 实测帧率、CPU、峰值内存和音频稳定性；三类原生存档通过 | 独立构建、产物检查、既有主机回归及生产适配层 MIPS 五模式贯通通过；实板验收待完成，尚未放行 |
| 6：引擎选择 | 菜单选择和单引擎资源生命周期 | 同固件内存预算可行，保存后释放旧引擎再启动新引擎，存档隔离正确 | 阶段 5 结果决定 |

阶段 4 中的小 ROM → `test4` → 32MiB ROM → 小 ROM → 退出，需要比较内存回收；原生存档覆盖 A / B 回退以及失败保留。30 分钟测试必须用拆分后的实际镜像，分别记录音频、显示、TF、保存和内存结果。mGBA 首轮采用独立存档目录，原生 `.sav` 双向验证后再考虑共用，即时存档始终按引擎隔离。

## 阶段 0 记录的边界

本次只增加文档和摘要，不改变固件行为。日志、镜像、ROM、存档和备份仍按仓库规则保留本机；可提交的摘要在 `docs/validation`，其中本机 `build/` 路径用于追溯，不表示这些产物已上传。后续每阶段完成时补充对应提交、测试日志与未通过项，阶段 4 完成前不将 mGBA 标记为已接入。

## 阶段 1：公共命令接口

新增不依赖 ReGBA 头文件的 `emu_engine.h`，控制台统一通过该接口提出载入、运行、保存、菜单、停止和音频请求。当前实现转发给原任务，保持命令格式、队列和资源生命周期；接口注明异步请求、路径复制、连续运行以及诊断调用的语义。

完整主机回归通过，Release / Debug 构建及产物检查通过；初始可用堆仍分别为 46764032 / 46108672 字节。Release BIN 为 2306448 字节，SHA256 `30799685ad915fc10a8182ff71efe106c459760b916884df74b076bf03db0008`；Debug BIN 为 2961904 字节，SHA256 `13a07b8be4cde270e35bee973890d7dad25e045bff8d22afff85211b253b1996`。本机日志为 `build/engine-stage1-{host,Release,Debug}-20261018.log`，本阶段没有板测。

## 阶段 2：归拢 ReGBA 实现

将原实现完整迁至 `engines/regba/regba_adapter.c`，并同步构建列表、字库输入和生产函数回归的来源。本次为行为保持的目录迁移，ReGBA 回调、JIT、ROM、BIOS 和存档私有状态已集中；其中任务与 GUI 调度将于阶段 3 通过同步 backend 合同移出。没有引入第二份 ROM、帧缓冲或任务。

完整主机回归、Release / Debug 构建和产物检查通过，初始可用堆与阶段 1 一致。本机日志为 `build/engine-stage2-{host,Release,Debug}-20261018.log`。本阶段没有实板操作，功能分离的板测仍由阶段 4 放行。

## 阶段 3：公共会话与同步后端

[`emu_session.c`](../projects/halley7m-regba/core/emu_session.c) 接管单个 FreeRTOS 模拟任务、命令队列、GUI、输入合并与松键门控、音频硬件启停、跳帧和倍速时钟。它只调用 [`emu_backend.h`](../projects/halley7m-regba/core/emu_backend.h) 中的同步操作与快照，不读取 ReGBA 全局。`emu_engine.c` 仅选择默认 ReGBA 后端，旧 `regba_core.h` 已移除。

ReGBA 适配层持有客体、JIT、ROM 文件和缓冲、RGB555 帧、PCM 未消费尾部、原生存档状态。`run()` 阻塞执行，帧回调返回停止意图后，由适配层自己的 `longjmp` 退出；视频 stride 以像素计，PCM 为带采样率的交错双声道有符号 16 位样本，部分消费由适配层保留尾部。未新增模拟任务、ROM 副本或帧缓冲。

停止分为 `run_quiesce` 和 `run_end`，先结束统计、停音频，再算最终 CRC 和等待保存。结束保存期间新到的保存请求在发布 `running=0` 前仍交给当前后端处理；菜单仅等待已有快照，失败仍保留当前客体。生产函数回归已迁移到新来源，补充 PCM 部分消费、回调返回后退出、真实 `run_game` 停止顺序及晚到保存请求断言。

完整主机回归、Release / Debug 构建与产物检查、MIPS 模拟器核心诊断通过。Release BIN 为 2308336 字节，初始可用堆 46759936；Debug BIN 为 2964528 字节，初始可用堆 46104576。相对基线，链接器按页对齐后的初始堆各减少 4096 字节；32MiB ROM 的实际运行余量由阶段 4 重新测量。完整哈希和日志见[阶段 3 软件验收摘要](validation/engine-separation-stage3-20261018.json)。MIPS 模拟器使用其独立软件 harness，适配层和公共会话的贯通则由主机生产函数夹具及后续实板验证覆盖。

本阶段尚未以重构镜像完成实板回归，保留 NOR 中原 GUI 固件。阶段 4 采用归档 Release 镜像先加载 RAM，全程静音；通过新镜像的完整门禁后，才开始 mGBA。

## 阶段 4：回归工具准备与执行

新增 [`test-engine-gui.py`](../scripts/test-engine-gui.py)，支持 `options`、`reload`、`large`、`switch` 四种模式，直接复用现有串口与存档诊断支持，不依赖本机 `build` 中的旧脚本。检查覆盖当前七项菜单、设置页持久选择行、静音实际 PCM、三类保存 A / 未保存 B / 读取 A，以及小 ROM → 龙珠 → 32MiB ROM → 小 ROM → 退出和内存回收。商业游戏只检查原生文件与缓存重启，不声称覆盖剧情点恢复。

串口、音频生命周期、CPU 采样和长测脚本现已支持单行中文路径；ROM 参数按 UTF-8 编码后的 500 字节限制检查。离线检查涵盖中文与 ASCII 的 500 / 501 字节边界、空白、换行和编码错误，且以替代串口核对实际发送的 UTF-8 字节，不打开实板端口。

MIPS 深度回归另通过 32 个 ARM 分支块并确认翻译峰值为 8、返回后深度为 0，日志为本机 `build/regba-sim-depth/simulator.log`。阶段 3 的真实 `run_game` 夹具还验证了反例：将最终 CRC 移到停音频之前，会触发断言失败。

工具准备不等于实板通过。RAM 加载使用已归档的阶段 3 Release；板测首条控制命令设置 `audio silent`，各轮结束停止核心与音频并等待保存完成，保持原 NOR 镜像。

### 阶段 4 实板验收：通过

2026-10-18～19 已将重构版 Release 经 U-Boot 加载到 RAM，核对 **2308336 字节 / CRC32 `07c8de28` / SHA256 `6d5a7e99a36c812776d7427c9ae3b2e8edf29666256f42ac1f3d57b8bfb378e6`** 后执行。阶段 4 完整主机回归也已通过。本轮未进行新的 NOR 写入；批次日志文件名继续使用 `20261018`，准确范围见[阶段 4 验收摘要](validation/engine-separation-stage4-20261018.json)。

| 检查 | 本轮实际结果 |
|---|---|
| JIT 同地址重写 | 37 → 73、跨 tick，实板通过 |
| 核心诊断与清缓存重译 | 两段各 120 帧，CRC 都为 `19b3e686`，IRQ 118 → 238，通过 |
| 三类 GUI 原生读档 | SRAM 32768、Flash 65536、EEPROM 512 字节，均完成保存 A/10 → 未保存 B/11 → 缓存 ROM 重启 A/10，重启后 `writes=0` |
| 菜单不覆盖旧档 | B 保留 dirty 时卡上仍为 A，读取后完整文件 CRC 与 A 一致，保存计数未增加 |
| 三类普通保存往返 | 自动保存、手动保存、停止落盘及分别重载全部通过；本轮累计 SAVE 12/12、错误 0 |
| 内存与正常收尾 | 最终空载游戏库堆 39073416 字节，与预热后基线完全相等；任务数 8，核心 / 音频已停止，音频静音，待保存状态全清空 |

首个空载启动样本堆为 39073848 字节；核心诊断完成并卸载后为 39073416，差额 432 字节。后续三类 GUI 读档与普通往返正常卸载后均回到该预热基线，不将仍载入诊断 ROM 时的临时占用计为泄漏。诊断收尾设置为音量 100、跳帧 0、速度 1×，LCD 欠载、音频队列 / FIFO 欠载、DMA 错误 / 超时及保存错误均为 0。

上述内置诊断的 `nonzero=0`，单独不能作为真实游戏非零 PCM 验收。用户已确认 TF 上均为可覆盖的测试存档，随后以实际商业 ROM 完成以下静音短测：

| 检查 | 新取得的实际结果 |
|---|---|
| 中文 GUI 与游戏设置 | 55 个状态样本，通过音量 0～100、跳帧 0～3、1× / 2× / 4×及恢复 1×；帧率分别为 59.8 / 119.4 / 238.8fps，恢复后为 59.8fps，实际 PCM / DMA 恢复且保持静音 |
| 龙珠保存与缓存读档 | 32768 字节 `.sav` 完整 CRC 为 `f4715198`，缓存重启约 0.229059 秒；新增保存成功，累计 SAVE 13/13 |
| 四次 GUI 启动和切换 | 8518208 字节蓝宝石 → 14357872 字节龙珠 → 33554432 字节究极绿宝石 → 蓝宝石 → 退出，21 个状态样本通过；退出堆 39073416，与起点相同，无额外滞留 |
| 32MiB 缓存读档 | 128KiB Flash 文件 `d5343e3a` 读档前后完整 CRC 一致；缓存重启约 0.1965095 秒，无完整 ROM 重读，运行堆 5517944 字节 |
| 实际游戏音频生命周期 | 36 个观察点、5 轮普通关闭 / 恢复和 5 轮快速 off / silent、启动取消、稳定等待及 stop / run 均通过；确认非零 PCM，每个样本静音且六项音频 / 显示 / 保存错误全为 0 |

音频生命周期中，ROM 常驻时堆为 24687224 字节；输出停止后仍保留 ROM，不能与空载库堆直接比较。最后一段 DMA 已处理非零 PCM 36855 个输入帧，周期最大发布间隔在本轮观察点中为 23276µs，DMA 最大重装耗时为 7µs。这些是本轮软件观测值，不代表扬声器听感。

本机新增 `build/engine-stage4-{options,switch,audio}-20261018.log/.json` 分别保存上述短测。随后，同一重构版镜像完成 30 分钟静音真实非零 PCM / LCD / TF 联合测试，用户确认“**画面和动画正常，无明显异常**”。阶段 4 的约定门禁现已通过；mGBA 在本验收快照中尚未开始。

| 联合长测及收尾 | 实际结果 |
|---|---|
| 时长与状态 | 主机 1800.268 秒、板端 1800.228989 秒；35 份完整状态，31 份正式运行状态；解析无异常项 |
| 正式区间帧率 | 游戏帧 134 → 107657，增量 107523，平均 59.7274017fps；LCD 刷新约 30.27226Hz |
| 静音音频 | 非零 PCM、输入 / 输出 / DMA 计数持续增长，音频队列 / FIFO 欠载、DMA 错误 / 超时全部为 0 |
| TF 写读 | 新建 SDTEST31～35 五个 128KiB 文件，写入 / 同步 / 回读全部为 `29df376d`；旧 SDTEST00 两次完整 CRC 复核通过 |
| 保存状态 | 全程 SAVE 13/13、错误 0，dirty / in_flight / requested 都为 0；本次正式长测没有新原生保存提交，保存写入另由短测证明 |
| 内存与栈 | ROM 常驻堆始终 24687224 字节，核心栈最小剩余始终 159784 / 245760 字节 |
| 文件复核及卸载 | 长测后龙珠 `.sav` 为 32768 字节 / `f4715198`，末个 SDTEST35 为 131072 字节 / `29df376d`；存档无残留 `.tmp`，正常 GUI 退出后堆恢复 39073416 字节 |

正式区间排除准备和停止尾部；停止时累计游戏帧为 107669，不能把尾部帧数混入正式区间平均值。收尾核心 / 音频均停止，保持静音，回到空载游戏库，音量 100 / 跳帧 0 / 速度 1×，SAVE 13/13 无待落盘修改。原始日志为 `build/engine-stage4-soak-20261018.log`，只读解析汇总为 `build/engine-stage4-soak-summary-20261018.json`，最终文件复核和释放证据为 `build/engine-stage4-post-soak-20261018.log/.json`。

本机早期实证为 `build/engine-stage4-{ramload,host,core,reload,save-roundtrip,cleanup}-20261018` 的日志或 JSON，三类保存诊断仅修改 `__save_*` 自有文件。商业游戏具体剧情点恢复仍不在本轮结论内；全过程静音、无新 NOR 写入。

## 阶段 5：mGBA 独立单引擎构建，验收进行中

2026-10-19 在阶段 4 放行后开始接入 mGBA，固定官方 **0.10.5 / `26b7884bc25a5933960f3cdcd98bac1ae14d42e2`**，源码位于 `third_party/mGBA` 子模块。当前交付是构建时选择一个引擎，运行时菜单选择和同固件双引擎仍属于阶段 6。

### 构建与资源边界

[`CMakeLists.txt`](../projects/halley7m-regba/CMakeLists.txt) 新增 `EMU_ENGINE=regba|mgba`，默认仍为 ReGBA；[`emu_engine.c`](../projects/halley7m-regba/core/emu_engine.c) 只选择对应同步后端。可通过 `scripts/build-regba.ps1 -Engine mgba -Configuration Release` 构建 mGBA，输出至 `build/mgba-Release`；Debug 使用 `build/mgba-Debug`。两个引擎均沿用 `halley7m_regba.bin/.elf` 文件名，RAM 加载显式指定本轮归档文件，避免混用默认产物。

mGBA 使用上游 Generic 静态库，只启用 GBA 内核；关闭桌面前端、线程、网络、调试器、脚本、档案、PNG 和多媒体依赖，并排除 Generic 默认加入的桌面 extras。像素采用 `COLOR_16_BIT`，不定义 `COLOR_5_6_5`，与现有 RGB555、240×160 输入及竖屏显示接口一致；`MINIMAL_CORE=2` 与禁线程配置在库和适配层一致。编译继续使用 MIPS32r2 / o32 / fp32 硬浮点 / little-endian ABI。mGBA 固件不链接 ReGBA 核心或两块 JIT 缓存，产物校验确认不存在 ReGBA JIT 符号；mGBA 的 `jit-test` 返回 `-ENOTSUP`，不作为解释执行内核失败。

LCD、音频硬件、TF/FAT32、GUI、输入、帧节奏、后台保存、字库和原创诊断 ROM 沿用公共层。mGBA 当前使用其内置 **HLE BIOS** 并跳过开机动画，没有加载 ReGBA 的 open BIOS 或用户 TF BIOS。ROM 仍从公共游戏目录读取，原生存档单独位于 **`/GBA/SAVES/mGBA`**，不写入已有 ReGBA `.sav`；跨引擎共用存档尚未启用。

### 适配实现及内存所有权

[`mgba_adapter.c`](../projects/halley7m-regba/engines/mgba/mgba_adapter.c) 实现同步加载、运行、卸载、输入、RGB555 帧、44.1kHz 交错双声道 PCM、状态快照及原生存档操作。公共会话继续处理音频硬件启停、帧节奏、跳帧和倍速；后端保留未消费 PCM 尾部。

mGBA 运行日志通过静态 logger 只保留 FATAL / ERROR / WARN，避免 BIOS SWI 的 DEBUG 信息持续占用 115200 串口。生产适配层的软件贯通还纠正了首帧退出条件：`hardCrash` 是上游的非法访问 abort 策略，默认值为 true；适配层以实际 `frameCounter` 是否推进判断一帧是否完成。

ROM 只分配一份缓冲，普通镜像按至少 2MiB 的 2 次幂容量补 `0xFF`，再用借用内存的 `VFileFromMemory` 交给 mGBA，避免上游为非 2 次幂镜像另分配 32MiB。**1MiB 镜像保留上游 Classic NES 特例**：单份 4MiB 缓冲中复制四份原镜像，再作为 4MiB VFile 映射。映射容量大于源文件时，在加载成功、重置客体前，以源文件实际字节恢复 `romCrc32`，保留上游依赖原始 ROM CRC 的硬件兼容判定。卸载先由核心关闭 VFile，再由适配层释放唯一 ROM 缓冲。

SRAM、Flash、EEPROM 三类原生存档已接入既有快照后台，支持 512 / 8192 / 32768 / 65536 / 131072 字节格式。mGBA 不持有磁盘存档 VFile，自动落盘统一经过公共保存服务；菜单只等待在途快照，显式保存、普通停止、切换和退出使用同步落盘。读档先完成文件读取与长度检查，再复用已映射 ROM 重置客体；填满 `0xFF` 的 128KiB staging 缓冲转交原生存档芯片 RAM，由其释放，避免重置后再分配失败。这里记录的是实现范围，三类实卡保存与恢复仍须分别验收。

原创 EEPROM 诊断原先用 CPU 逐 bit 写入 halfword，mGBA 的原生芯片模型则依据 DMA 传输剩余长度识别 EEPROM 地址宽度；Flash 诊断原先也没有等待擦除 / 编程完成，软件探针证实忙状态读值会污染诊断 scratch。生成器现提供可选 `--eeprom-dma` 和 `--flash-poll`，仅 mGBA 的资源准备同时启用 DMA3 EEPROM 协议与 Flash 完成轮询；诊断保存 payload、mailbox 和验收标记保持一致。默认 ReGBA 的三个诊断 ROM 及生成资源头逐字节不变，新准备流程也已重新构建并核对默认 Release / Debug BIN 身份。

### 已取得的软件证据

最终冻结源码的四种构建及产物校验通过。以下“初始堆”为首次任务分配前的链接器预算，不能当作运行峰值或 32MiB 游戏可用余量：

| 引擎 / 配置 | BIN 字节数 | 首任务分配前堆空间 | SHA256 |
|---|---:|---:|---|
| ReGBA Release | 2308336 | 46759936 | `6d5a7e99a36c812776d7427c9ae3b2e8edf29666256f42ac1f3d57b8bfb378e6` |
| ReGBA Debug | 2964528 | 46104576 | `06a1be117c26c3e235b33bda16ee24bd6615dc322a4692e51db3c9816db24554` |
| mGBA Release | 2386540 | 58155008 | `b13fa27e635421662e3ac0f92fd17c374c24d4892a7e4112ed381eb530341ab1` |
| mGBA Debug | 2745100 | 57794560 | `d088ebbed89b697a481ec9d242427a5cdbecd5cce5c2899a8e2fccaff3d3146d` |

默认 ReGBA 的 Release / Debug BIN 均与阶段 4 的对应产物逐字节相同。mGBA 与 ReGBA 的引擎身份校验还分别验证了反例，混用引擎参数和固件时均被拒绝。四种产物的 BIN / ELF / MAP / manifest，以及关键源码哈希，保存在本机 `build/firmware-revisions/engine-stage5-final-20261019/`，汇总为其中的 `validation.json` 及 `build/engine-build-switch-validation.json`；这些本地产物不随文档上传。

最终源码的完整主机回归通过，日志为 `build/engine-stage5-host-final-20261019.log`，包含迁移后的生产会话 / ReGBA 函数、驱动模型、保存、GUI 与 UTF-8 工具检查。另新增 [`test-mgba-sim.py`](../scripts/test-mgba-sim.py) 和 [`mgba_sim.c`](../tests/mgba_sim.c)，直接编译生产 `mgba_adapter.c`，调用实际 backend 操作，以软件 host、文件与保存 I/O 桩对接真实 mGBA 静态库，并使用官方 Linux MIPS 模拟器执行。以下五个模式均正常退出，`inputs_changed=[]`：

| 软件模式 | 已通过的检查 |
|---|---|
| CPU | 实际 `ops.selftest()` 运行 120 帧，ARM / Thumb / SWI / IRQ / RAM-code / SMC、六个 mailbox 与整帧像素通过；IRQ 114，PCM 回调累计 87973 帧，解释器 JIT 诊断返回 `-ENOTSUP` |
| SRAM | 32768 字节完整 A / B / 恢复 A、读取失败保留 B 与 dirty、重新加载、128KiB staging 所有权及未用尾部通过；保存 A CRC `5868628d` |
| Flash | 65536 字节完整 A / B / 恢复 A、读取失败保留 B、重新加载与 staging 检查通过；原生 Flash 升级为 131072 字节并保持新增区域 `0xFF`；保存 A CRC `1c69ce88` |
| EEPROM | DMA3 诊断的 512 字节完整 A / B / 恢复 A、读取失败保留 B、重新加载与 staging 检查通过；原生 EEPROM 升级为 8192 字节并保持尾部 `0xFF`；保存 A CRC `248d5c5d` |
| 1MiB 镜像 | 单份 4MiB 缓冲的四段完整镜像、源文件 CRC `394f127d` 与随后 120 帧执行通过 |

五份正式结果使用相同 adapter / harness / 静态库 / 资源输入，资源头 SHA256 为 `311b3a3fb98595354374b3a7d3406e24a708d0a969cd960d4650010b9309f722`，与最终构建一致。原始日志和结果位于本机 `build/mgba-adapter-sim-{cpu,sram,flash,eeprom,mirror}/simulator.log` 与 `result.json`，归档 `validation.json` 同时记录五项机器证据。软件模式完成了生产后端与 host 合同的贯通；实际 FreeRTOS 调度、LCD / DMA、TF 卡写入和真实非零 PCM 仍由实板验收。模拟器在电脑上的运行耗时不换算为板上 FPS。

可公开追溯的构建身份、CRC 和五项软件验收摘要见[阶段 5 软件验收摘要](validation/engine-separation-stage5-20261019.json)。该摘要明确区分软件通过和实板待验收。

### 尚待放行的验收

阶段 5 当前**尚未放行**，纯软件贯通已完成。剩余门禁是在 RAM 实板测量真实游戏帧率、主核 CPU、运行内存与正常卸载回收，验证 32MiB ROM、三类 TF 原生存档保存 / 旧档回退 / 重载，以及 LCD、静音真实非零 PCM / DMA、TF 读写的联合稳定性。原生 `.sav` 的跨引擎双向互通、商业游戏剧情点恢复及引擎菜单选择不计入当前已通过范围。板测继续保持静音并保留原 NOR，不能以阶段 4 的 ReGBA 结果替代 mGBA 验收。

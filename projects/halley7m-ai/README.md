# HALLEY7-M 云端 AI 终端

独立的 X2600M MIPS + FreeRTOS 工程，入口 `0x80600000`，54 MiB 主核 RAM，
RISC-V 保持复位。按源文件复用播放器的启动、显示、GT911、按键和 TF 适配。
目前是可构建的基础固件，网络尚未接通，点击发送会保留输入并提示未连接。
电脑端 DeepSeek 真实文字测试通过不代表板端已经能聊天。

## 当前交付与阶段边界

| 工作包 | 已完成 | 尚未完成 |
|---|---|---|
| P0 云协议 | DeepSeek原生HTTPS/SSE、12轮文字与8轮历史裁剪；百炼WSS离线探针 | 百炼真实混合语音和instructions历史验证 |
| P1 固件基础 | MIPS构建、ELF/BIN核验、中文键盘/候选、TF资源与配置加载；RAM实板显示/触摸/Start由用户确认正常 | 缺失/损坏资源路径实板、音量键交互 |
| 会话核心 | 50轮显示存储、独立8轮完整上下文、手动重试与旧代次丢弃 | 会话任务、聊天记录页面、实际流式回答接入 |
| 音频核心 | Air1103拆包组帧、带低通24→16kHz重采样、PTT边界 | UART接线/DMA、录放音与实际尾音测量 |
| 网络基础 | 双网选择策略、lwIP2.2.1原生FreeRTOS适配；实板DTRNG有限采样、PHY地址0/ID02430c54读取 | GMAC帧收发、持续熵服务、DHCP/DNS/SNTP、真实TLS/WSS、SWT6621-SH |
| TLS适配 | Mbed TLS4.1.1客户端、强制证书校验、TLS1.2/1.3本机真实握手及异常证书拒绝、MIPS编译/可重定位链接 | 绑定板端socket、熵/UTC/CA服务及真实云握手 |
| 发布 | 资源散列、构建产物校验 | 双网长跑、NOR安装与回读、物理断电冷启动 |

前置硬件材料和阻塞项见 [BOARD_PREREQUISITES.md](BOARD_PREREQUISITES.md)，
每种验证的实际结果见 [VALIDATION.md](VALIDATION.md)。没有用模拟回答或空驱动代替云端功能。

## 准备与构建

沿用仓库 XBurst2 Windows 工具链、CMake/Ninja 与 Python。首次准备词库需要联网，
普通构建不自动联网。词库固定为Rime提交 `0c6861ef7420ee780270ca6d993d18d4101049d0`，
输入字节校验SHA256；生成器只保留字体支持的GB2312单字和最高频20,000个2～4字词组。
输入全拼可连写，例如 `nihao`，按固定词频显示和翻页，不提供整句智能分词。

```powershell
python scripts/prepare-ai-resources.py --download
.\scripts\build-ai.ps1
python scripts/test-ai-host.py
python -m unittest discover -s tests -p 'test_ai_*probe.py' -q
python scripts/prepare-ai-network.py --download
python projects/halley7m-ai/network/tests/check_build.py --mips
python projects/halley7m-ai/tls/tests/check_build.py --mips
```

产物为 `build/ai-Release/halley7m_ai.{elf,bin,map}` 和 `manifest.json`。
核验检查ELF入口、MIPS小端、加载段边界与BIN内容，并单独报告是否适合现有2,326,528 B NOR应用区。
镜像容量合格不等于允许烧写，首轮仅RAM装载；本项目尚未制作NOR安装包。

主机测试执行真实C模块与LVGL，包含帧表面所有权、中文候选选择、键盘/退格、失败保留输入、
历史淘汰与请求代次、资源损坏、音频协议分块和重采样频响。界面截图保存在
`build/ai-host/gui-screenshots/`；主机测试不能替代实板触摸或刷新验收。

## TF 资源与配置

生成目录 `build/ai-resources/ai/` 包含 `font.aif`、`pinyin.aip`、配置模板、
`SOURCE.json` 和许可证。字体34,869字形/1,255,300 B，词库27,291条/652,161 B。
两个资源均含版本魔数、条目数、长度和CRC32；开机同一个任务串行读取到RAM，随后GUI只查RAM。
固件还内置页面和资源错误提示所需字形。运行中不并发读取词库/字体与无线资源。

手工复制为TF根目录 `/ai/`，并把配置模板改为 `/ai/config.ini`。
也可使用以下脚本，在指定TF根目录**新建**`ai`，不覆盖任何既有目录，完成逐文件回读校验：

```powershell
python scripts/install-ai-tf.py --card G:\ --key-file configs/config.txt
```

`--key-file`支持与电脑DeepSeek探针相同的本机文件格式；不传时只安装模板。
实际配置使用UTF-8 `key=value`，选项见 [config.example.ini](config.example.ini)。
当前provider为 `deepseek`、model为 `deepseek-flash`；Wi-Fi选项暂留空。
配置中的API Key、Wi-Fi密码不会输出，`configs/config.txt`与`configs/ai.local.ini`已忽略。
TF中的密钥是设备需要读取的明文配置；它不进入公开资源包或源码提交。

## RAM 联调与诊断

确认COM52确实是目标开发板后，可复用现有RAM装载器；以下命令先识别当前播放器，
复位至U-Boot、验证时钟、YMODEM加载并核对长度/CRC，最后跳转，不写NOR或saveenv：

```powershell
python scripts/ramload-regba.py --port COM52 --file build/ai-Release/halley7m_ai.bin --execute --reboot-player
```

已运行AI固件时改用 `--reboot-ai`；该路径同样先确认AI状态标识再复位。

板端日志使用 `AI_BOOT` / `AI_STATUS` / `AI_RESOURCES`，控制台提示符为 `ai>`。

| 命令 | 功能 |
|---|---|
| `status`、`heap` | 当前基础阶段、资源结果、GUI循环及剩余堆；只显示key是否存在 |
| `buttons`、`display` | 现有三键和显示状态 |
| `net rng` | 256个DTRNG有限采样诊断，不输出原始随机字、不注册TLS熵 |
| `net phy` | 显式复位PHY并扫描MDIO，结束恢复引脚/时钟；不启动帧DMA |
| `reboot` | 看门狗复位，返回原有NOR启动程序 |

`net phy`短暂占用现有PC15～PC24以太网引脚；严禁把Air接到占用GT911的J6 PC7/PC8。
详细寄存器证据、超时边界和待确认差异见 [network/README.md](network/README.md)。

## 内存与后续接入

显示历史约1.76MiB；模型8轮独立快照约288KiB，计入8MiB网络/协议预算。
独立快照保证连续失败或取消不会把最近完整问答从模型上下文挤掉。
失败或取消的残片保留原状态；手动重试另建显示轮并使用新代次，重复点击不重复提交。
音频计划分配1MiB录音、2MiB重采样后播放缓冲，实际UART驱动尚未接入。
GUI约使用两张3.52MiB的ARGB原生表面、160KiB行缓冲及1MiB LVGL堆。
当前不能把静态链接大小或空载剩余RAM当成两小时联合峰值验收。

许可证保留在 `licenses/`；Rime词库为Apache-2.0，现有ReGBA Unifont来源为GPL-2.0-or-later。
字体/词库变换由 `scripts/prepare-ai-resources.py` 可复现，未使用完整Rime引擎。
FreeRTOS、LVGL与现有平台源保持仓库原许可证，lwIP/Mbed TLS版本及许可证见网络依赖清单。

DeepSeek纯C请求/SSE协议层见 [cloud/README.md](cloud/README.md)。它已通过11组主机用例和MIPS静态库编译，
尚未连接设备TLS/HTTP传输；该静态库不等于主程序已具备联网能力。

TLS模块独立构建和接入约定见 [tls/README.md](tls/README.md)。本机回环测试使用Windows BCrypt提供熵，
板端尚未安装该回调；不会以PC熵或固定seed代替X2600持续随机数服务。

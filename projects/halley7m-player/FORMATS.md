# 直接播放格式扩展

本阶段接入 MP3、AAC/M4A、MP4（H.264＋AAC），普通 1280×720 横屏 MP4
在板端旋转。MIPS 解封装、解码及转换，RISC-V 消费 48kHz / S16LE / 单声道
PCM。默认音量仍为7%。真实TF十二个压缩音频组合和两份横屏MP4已完成短片板测：
两份30秒MP4各900帧全呈现、drop/late0、1440000有效样本完整且音频错误0。
逐例证据见[音频实板摘要](audio-decode/BOARD_VALIDATION_20261020.md)和
[显示实板摘要](display/BOARD_VALIDATION_20261020.md)，分别保留固件身份；
新MP4的暂停时钟冻结、恢复、播放中菜单/诊断自动按键周期及wide/native回切也已通过。
不沿用旧AVI的30分钟记录证明新解码器或旋转器的长期性能。

## 当前输入范围

| 文件 | 当前会话支持 |
| --- | --- |
| MP3 | MPEG Layer III，有界帧长；ID3v2.2/2.3/2.4、ID3v1；CBR/VBR，Xing/Info 与 LAME 延迟/尾填充 |
| AAC | ADTS AAC-LC，1024 源样本帧，单个 raw_data_block；无精确尾时长时保留实际解码尾填充 |
| M4A | 非分片 MP4 中的 AAC-LC，使用 ASC 和简单 edit list 在源样本轴裁剪 |
| MP4 | 一个 H.264 Baseline/Main/High 视频轨＋一个 AAC-LC 音轨；8bit420逐行，真实 PTS/DTS、CTTS、CAVLC/CABAC、B 帧，参考帧≤6、声明重排深度≤4、DPB≤6；常见尺寸按 coded 像素预算接入 |
| AVI/WAV | 原固定 H.264＋PCM AVI/AVIX 与 48kHz 单声道 S16LE WAV |

压缩音频接受 8–48kHz、单/双声道；真实片源主机矩阵覆盖 44.1/48kHz。
双声道算术平均后，用连续有理相位线性插值输出 48kHz 单声道。HE-AAC/SBR/PS、
多声道、自由码率 MP3、分片/加密 MP4、复杂 edit、超过 coded 像素预算的尺寸
不在本轮整机范围。底层 MP4 解析器能报告更多时序，并不代表会话和硬解已经
支持。普通横屏文件无需预旋转，仍须满足上述编码范围。进度跳转尚未接入。

MP4 时间戳已贯通 `demux → source → codec 帧 → 显示调度`，不再以帧号除以30
重建。PTS/DTS/包时长使用有符号微秒；旧 AVI/WAV 按绝对有理数边界计算。
24、25、30000/1001fps 及33/34ms量化时间轴通过逐包 FFprobe 和真实会话
主机回归。第二步已验证 CABAC、Main/High 和1/3/6参考帧：630帧硬解Y/UV
逐帧与PC一致，正常GUI/7%有声播放630帧全呈现、音频1008000样本完整且错误0。
第三步已接通 CTTS、非零 video edit、POC 输出重排和 EOF 排空。解码序号与呈现
序号分别保留，末帧由呈现窗口的最大 PTS 判定；窗口外 preroll 继续解码以保持
参考链。暂停冻结音频样本时钟和呈现；音频早于视频结束时补齐时间轴静音，并
单列补齐样本数，DMA 周期末的硬件补零仍不计为媒体样本。不支持10bit、
422/444、隔行/MBAFF/FMO。

九张线性 NV12 与内部 Tile DPB 分开计入预算。内部控制、运动信息和 Tile Y/UV
采用一次分配的九槽工作区；720p 共16,853,760B，避免逐帧大块分配造成碎片。
SDK 只访问当前帧实际启用的参考列表，停止后工作区活动数必须归零。具体板测
内存和像素正确性以当前固件的 VALIDATION 记录为准，不据此承诺1080p或更多
参考帧。

### 尺寸、SAR与颜色

可见宽高为正偶数、单边不超过2032；分别向上对齐到16后，coded像素数不超过
1280×720。NV12按coded高确定UV起点、按128对齐stride，不把可见高当成平面边界。
本轮样片覆盖320×240、640×360、640×480、854×480、960×540、1280×720、
原生720×1280，以及720×576/SAR16:15。常见右/下宏块补齐被裁掉；例如
640×360实际分配640×368，854×480为864×480，960×540为960×544。
SPS coded矩形必须等于预分配矩形，任意非最小coded画布的左/右裁剪不作通用支持。

按SAR计算显示比例，在固定横屏1280×720或原生720×1280画布内等比居中，空余
区域留黑。640×480与720×576/SAR16:15均显示为960×720、左右各160像素黑边；
854×480等接近16:9的尺寸可能因偶数缩放取整留下极细黑边。中途改变分辨率、
裁剪、SAR或有效色彩格式仍明确退出，不能把新参数套用到旧帧内存。

保留SPS VUI与MP4 `colr/nclx`、`colr/nclc`、`pasp`原始信息，按每个属性独立
使用“SPS已知值→容器后备值→显示层推断”。DPU实现BT.601/BT.709各自有限/全
范围四组CSC；有限范围减Y16/UV128，全范围减Y0/UV128。未知矩阵按SD601、
HD709推断，未知范围按有限范围，诊断输出保留inferred标记。旧AVI契约继续
显式使用601full。PQ、HLG、BT.2020、ICC变换不在本轮范围。

不再仅因Level标签高于4.0拒绝。4.1/4.2样片只重标SPS/avcC，全部图像和AAC
载荷保持原3.1文件不变；通过它们不表示获得1080p、高DPB或更高帧率能力。

用户已选择兼容优先：显式加权预测中的默认 `+128` 超出 MCE 有符号8位接口时，
仅在硬件交界处映射为 `+127`，不修改容器和标准参数解析。诊断页显示“权重近似”
的输入帧数，串口提供 `weight_approx`；参考链可能使后续输出帧也受影响。
对原始 `test.mp4`，5个P帧的近似影响12个输出帧，PC对照的最大UV差为3/255，
理想RGB换算最大5–7/255；这些数值只对应该文件，不是通用上限或实屏测量。

`source.next → source.read → 字节入队` 在唯一媒体任务中同步完成，不能延后
读取未入队的源描述符。MP4 在分配前计算规范化包长，音视频压缩包均保留
64 字节零 padding。MP4/M4A 在 LOADING 显式建立有界 CLMT，包含不足 1GiB
的文件，避免样本表与数据间回退时重复遍历 FAT。

## 样本数与资源

压缩媒体以协议 v4 的未知总样本模式开始，EOF 在解码、裁剪、转换尾段和共享
写入全部完成后，以实际写入数封闭会话。界面播放中总时长为 `--:--`，结束后
显示实际时长；WAV/AVI 的已知总量检查继续生效。

FAAD2 内建跳过首 AU 的 1024 源样本，先计入原始样本轴，再应用 M4A edit，
避免剪两次。MP3 排除 Xing 元数据帧后，头裁 `encoder_delay+529`，正尾裁
`encoder_padding-529`。movie tick 造成的末尾不足一 tick 舍入与实际媒体
区间取交集，不生成缺失样本。

媒体任务保留 decoder-owned PCM，背压排完才解下一包；另有 16384 样本暂存
环。根据真实 MIPS `.su` 嵌套调用开销，原 16KiB 任务栈增至 96KiB；`heap`
导出 `media_stack_free_words` 供板测检查。旋转另用三张 CUSTOM 表面，共
11,059,200B；宽幅 GUI 存续时继续归显示层持有，切回 native 或整体停止后释放。

## 验证与运行

```powershell
python scripts/test-player-audio-decode.py
python scripts/test-player-mp4.py
python scripts/test-player-source.py
python projects/halley7m-player/mips/test-session.py
python scripts/test-player-gui.py
python scripts/make-player-format-fixtures.py
.\scripts\build-player.ps1 -Configuration Release
python scripts/test-player-formats-runner.py
python scripts/run-player-formats-board.py --dry-run
python scripts/make-player-geometry-fixtures.py
python scripts/make-player-level-fixtures.py
python scripts/make-player-color-reference.py
python scripts/test-player-geometry.py
python scripts/test-player-codec-format.py
python scripts/test-player-display-sample.py
python scripts/test-player-geometry-runner.py
python scripts/run-player-geometry-board.py --dry-run --check-csc
```

解码器 16 个真实片源、MP4 25 项、统一 source 24 个真实片源及 16 个错误用例
已通过；生产会话另验证四格式各三次暂停/恢复/EOF/STOP、准确输出计数，以及
旧 WAV/AVI 生命周期。headless 显示测试覆盖路线握手、写回/旋转/显示完成
持有关系和未知时长 UI。以上均为主机测试。

14 个 30 秒板测片源位于 `build/player-fixture/formats`，约 5MiB。只复制顶层
媒体：音频放 `multimedia/audio`，MP4 放 `multimedia/video`。manifest 保留 SHA、
参数及源/实际 PC 解码计数；`_sources`、`_reports`、`_host` 留在 PC。使用原 RAM
加载入口，软件复位先识别播放器，不写 NOR。用户已取消手机录像及实际声光
偏差数值验证，本轮不再要求补录。

片源复制并完成 RAM 加载后，可运行：

```powershell
python scripts/run-player-formats-board.py --port COM52 --root 0:/multimedia `
  --log build/player-formats-board-r1.log `
  --firmware-manifest build/player-riscv-Release/manifest.json
```

该入口逐个播放 manifest 中的14例，EOF 后核对实际封闭的有效样本、音频错误、
视频帧计数及栈余量，再STOP并确认IDLE/音频关闭；播放期间不集中查询串口。
`--file <完整文件名>`可重复选择子集，目录也可用`--audio-root/--video-root`
覆盖。日志和JSON报告要求新文件；全程仅设置7%，失败不自动重播、复位或上传。
drop/late单列为视频实时指标，不能用功能播放成功替代。manifest记录的是预期
主机固件，板端固件身份由独立RAM上传日志关联，不伪称runner查询了板端SHA。
10组假UART测试通过。共享Console另有15组回归，覆盖64B接收FIFO下长UTF-8命令
的分块节流，以及STOP回复类型/请求号检查。实板首次75B长命令丢尾未进入播放，
原失败日志保留；工具修复后，两MP4用同一固件的独立日志重新验收通过。

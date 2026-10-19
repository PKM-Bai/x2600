# 手机实拍声光同步分析

工具分析手机录像中屏幕 ROI 的提亮与 1500Hz 短音，不读取板内 PTS 来代替声光测量。
适用于受控片源每秒一次的白闪/短音，录像至少包含三组完整事件，首尾留暗画面和静音。
1、15、29 分钟附近各保留一个正常帧率录像片段，使用同一手机、拍摄模式和摆位；
避免慢动作导出、分别重置音轨/视频时间轴或去除拍手校准原始音轨。

```powershell
python scripts/analyze-player-sync.py C:/capture/1min.mp4 --roi 120,240,500,320 --label 1min --output build/player-sync/1min.json
python scripts/analyze-player-sync.py C:/capture/15min.mp4 --roi 120,240,500,320 --label 15min --output build/player-sync/15min.json --calibration-video-contact 0.533333 --calibration-audio-contact 0.553 --calibration-uncertainty-ms 17
```

ROI 格式为 `x,y,width,height`，使用录像**编码像素**，FFmpeg autorotation 关闭；
手机文件的旋转 metadata 保留在报告中，因此 ROI 须按未自动旋转的图像选取。
ROI 应只覆盖会提亮的屏幕区域，排除周围灯光、人物和常亮字幕。
自动亮度阈值采用 ROI 中位亮度与峰值差的 70%；必要时用
`--flash-threshold 180` 指定 0–255 均值门限。检测不到时报告错误，不能由此判定播放器失步。

默认每秒一个事件、允许间隔偏差 150ms，并要求声光数量相同、至少三组。
`--expected-pairs N` 可固定完整事件数。`--event-period`、`--period-tolerance`
可对应已知片源调整。配对按严格时间顺序，默认允许每对差不超过 400ms，
且窗口必须小于半个事件周期；漏闪、额外短音或缺失一整个周期均报错，不顺延补配。
若片段在闪光/短音中开始或结束，应保留完整边界后再测。

`raw_audio_minus_optical_ms` 正值表示录像中的声音晚于屏幕首亮帧。
校准需在同机、同模式、同几何的拍手录像上，手工选取可见接触时刻与声音起点，
并传入各自在该拍手录像共同 PTS 轴上的秒数：

```text
capture_bias = clap_audio_contact - clap_video_contact
corrected = recorded(audio - optical) - capture_bias
```

未给校准时，校准后值为 null；未给 `--calibration-uncertainty-ms` 时，手工校准
不确定度保持未知，不当成零。给定后报告额外扩展区间。拍手接触与纯音起点采用不同
检测方法，摆位/声传播、曝光和滚动快门限制仍保留，不能把校准当成所有误差已消除。

视频使用逐个解码帧的真实 presentation/best-effort PTS，VFR 不按 `index/fps` 换算。
每次提亮保留前暗帧与首亮帧的 PTS 区间。30fps 一帧约33.33ms、理想半帧约16.67ms；
60fps 分别为16.67ms和8.33ms，实际掉帧间隔由录像计算。名义光起点使用首亮帧 PTS，
报告区间表示采样分辨率，不冒充相机曝光时刻的统计置信区间。

音频先解码到原采样率单声道 float，以 ffprobe 实际解码帧 PTS 和样本数逐帧对应，
不会把音频起点单独归零，也不会把时间戳空洞压紧。4ms 窗、约2ms步长计算 1500Hz
正余弦投影 RMS 并要求窄带占比，避免因果带通群延迟；起点采用首个有效窗中心。
AAC priming/skip 后的解码样本数必须与 FFmpeg 实际 PCM 完全一致，否则拒绝分析。

JSON 保留文件 SHA256、工具版本、流时间基/起点、检测设置、全部声光事件与逐对原始/
校准后误差。失败仍写错误 JSON 并返回非零。`kind=real_capture` 表示输入录像测量；
合成测试必须用 `--synthetic`。工具始终输出 `physical_acceptance=not-evaluated`，
不会根据合成数据或单个名义数值自动宣称实际声光同步通过100ms。

验证：`python scripts/test-player-sync.py`，只在 `build/player-sync-tests` 生成本地
MP4/WAV/JSON，不播放声音。覆盖已知+80ms/−50ms、AAC非零首PTS、共同时间轴平移、
VFR、60fps、背景噪音、手工采集偏差，以及漏闪/多音/纯噪音/错误ROI的拒绝。

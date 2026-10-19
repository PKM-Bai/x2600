# 播放器只读 TF 适配

`player_storage` 复用 ReGBA 已验证的 SD-SPI、FatFs 与 UTF-8/UTF-16 路径转换，通过 `player_media_io` 给纯C解析器提供 read/seek/size。首个 mount 或显式内存诊断任务拥有后续所有操作，ISR/其他任务不能调用；当前一次只打开一个文件，不做热拔插自动重挂载。

派生的 FatFs 设置 `_FS_READONLY=1`，媒体小于4GiB。64位解析偏移在缩窄为 `FSIZE_t` 之前检查文件大小与FAT边界。`prepare-storage.py` 只生成build内副本，删除SD写入与SSI的PDMA生命周期/DMA入口；TF始终走polling，避免触碰由小核音频独占的主PDMA。

独立MIPS静态库已编译通过，`python projects/halley7m-player/storage/test-storage.py` 使用实际适配C代码与内存FatFs peer检查中文路径、只读标志、大小/偏移、任务归属、IO错误及目录关闭。该主机结果不代表TF实板挂载、带宽或连续播放已验收。

完整会话通过 `player_storage_dir_open/next/close` 按步遍历目录。一个只读文件与一个目录游标可以共存，但都只归首个 mount 的 worker；`next` 每次只执行一次 `f_readdir`，0 表示条目、1 表示目录结束。会话在音视频服务之间推进游标，GUI 只读取十条窗口快照，不对目录总数设置上限。主机回归已覆盖游标独占、跨任务拒绝、中文条目、EOF 和关闭后的失效句柄。

显式 `@embedded` 路径仅用于 RAM 整机联调。worker 用 `player_storage_register_embedded` 注册固件链接的不可变字节后，可通过同一 `open/io/close` 接口读取；TF 挂载失败也能使用，但仍占同一个唯一文件槽、遵守任务归属和偏移检查。其 read/seek/close 不调用 FatFs，不会出现在目录列表。生产 session 在 `PLAYER_HAVE_TEST_MEDIA=1` 时自动注册现有 `test_media.S` 的 start/end，console 可显式 `play @embedded`。主机已验证挂载失败时内存读取、EOF、越界、跨任务拒绝和打开期间不能替换片源；这不构成真实 TF 验证。

`player_storage_gpio_probe` 是显式卡侧诊断入口，要求同 worker，且没有成功挂载、打开文件或目录。它调用已复用的 GPIO mode 0 CMD0/CMD8 八次试验，不读写文件。成功挂载后返回 `-EBUSY`，避免诊断偷偷复位正在使用的卡。主机只验证调度器/任务归属和 mounted/openfile 拒绝边界；真正电气响应仍需板测。

`tf bench <UTF-8路径>` 在同一 worker、音频停止后只读文件前 8MiB（小文件读到其大小），使用固定 16KiB 缓冲，块间维护 AMP 心跳并让出一个 tick。`TF_BENCH` 的 `Bps` 包含块间让出时间，`read_Bps` 只按 `read_us` 统计；`min_read_us/max_read_us` 是每次读调用耗时，提前 EOF 标为 `truncated=1` 并报错。挂载、打开和关闭不计入吞吐窗口，`@embedded` 被拒绝，避免把内存速度当成 TF 速度。`python scripts/test-player-benchmark.py` 只验证生产函数的边界与错误关闭，不代表真实 SPI/TF 带宽。

## 大文件 CLMT 定位

实际跨 AVIX 的 2147591940-byte 受控片已能完整解析和播放到 ENDED，但首轮出现 90 decoded、82 displayed、8 dropped/late，最大提交迟到 120000us。这一轮不算 AVIX 播放性能通过。时序中的两次约 150ms worker 空档对应跨过大段 JUNK；普通 FatFs `f_lseek` 要逐簇遍历 FAT，不能把主机稀疏文件的快速 seek 当作 TF 时延。

`player_storage_prepare_seek(stream, pump, opaque)` 供 LOADING 使用，调用点在 TF 文件打开后、read-ahead/codec/audio 创建前。仅文件大小至少 1GiB 时申请 4096 个 DWORD（16KiB）并用 FatFs 自带 `CREATE_LINKMAP` 创建 CLMT。小文件和 `@embedded` 直接成功，不申请 map；普通 593MB 长跑片保留原来的顺序读与普通定位路径。

CLMT 每个连续片段占两个 DWORD，另外有长度和终止项，4096 项支持最多 2047 个片段。图满返回 `-E2BIG`，保留所需与容量统计，清除半成品并释放内存；不静默退回播放时的慢 seek。取消或 I/O 错误也清除半图。成功的 map 属于唯一文件，直到 close 才释放，不会增加所有会话的 BSS 或为小文件保留额外堆。

`_USE_FASTSEEK=1` 仅用于播放器派生 FatFs。`prepare-storage.py` 在 build 内的 `ff.c` 的 CREATE_LINKMAP 内层 FAT 步插入 `player_storage_clmt_step`，原 SDK/ReGBA 源文件不变。计数跨片段连续，每 256 次 FAT 步调用一次用户 pump 并 `vTaskDelay(1)`，初末也调用 pump，避免每簇等待一个 tick 的长启动。pump 返回非零可取消，且不能重入 FatFs；生产调用负责维护 AMP 并观察 STOP。图建立后，后续随机 JUNK/AVIX 跳转根据内存片段表定位，不逐次遍历 GiB FAT 链。

`player_storage_get_seek_stats(stream, &stats)` 返回 `enabled/capacity_entries/required_entries/used_entries/build_us/seek_calls/max_seek_us`；传 NULL stream 读取最近文件的统计。close 后保留统计到下一次成功 open，便于停止后诊断。`build_us` 单列建图成本，`seek_calls/max_seek_us` 记录实际定位调用，包含所需的数据扇区缓存读取。最大 4GiB-1 的 EOF offset 与 FatFs 的 CREATE_LINKMAP 哨兵值相同，适配器用末字节读取完成这一 EOF 定位，保留原来小于 4GiB 的偏移契约。

验证命令：

```powershell
python projects/halley7m-player/storage/test-storage.py
python scripts/test-player-fastseek.py
cmake --build build/player-storage-probe -j4
```

真实 FatFs 测试按扇区合成一个合法 FAT32 块设备，文件为 2GiB+108292 bytes，不在主机创建多GiB数据文件。连续链和两个片段分别得到 4/6 项 CLMT；跨 2GiB 正向与回退 seek 不增加 FAT I/O，数据映射正确。65540 个全碎片簇需要 131082 项，容量不足明确失败，pump 不因片段边界重置；首 pump 与中途取消均关闭半图，小文件与内存文件不建图。测试编译生产适配器与实际派生 `ff.c/unicode.c`，新适配器与测试使用 MSVC `/W4 /WX`；已有 vendor 对象单独编译。实际 MIPS 静态库也编译通过。CLMT 修正后的实板 AVIX 时延、掉帧与停止回收仍需重新验收。

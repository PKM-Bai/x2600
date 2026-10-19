# 播放器只读 TF 适配

`player_storage` 复用 ReGBA 已验证的 SD-SPI、FatFs 与 UTF-8/UTF-16 路径转换，通过 `player_media_io` 给纯C解析器提供 read/seek/size。首个 mount 任务拥有后续所有操作，ISR/其他任务不能调用；当前一次只打开一个文件，不做热拔插自动重挂载。

派生的 FatFs 设置 `_FS_READONLY=1`，媒体小于4GiB。64位解析偏移在缩窄为 `FSIZE_t` 之前检查文件大小与FAT边界。`prepare-storage.py` 只生成build内副本，删除SD写入与SSI的PDMA生命周期/DMA入口；TF始终走polling，避免触碰由小核音频独占的主PDMA。

独立MIPS静态库已编译通过，`python projects/halley7m-player/storage/test-storage.py` 使用实际适配C代码与内存FatFs peer检查中文路径、只读标志、大小/偏移、任务归属、IO错误及目录关闭。该主机结果不代表TF实板挂载、带宽或连续播放已验收。

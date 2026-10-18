# ReGBA 来源

`ReGBA` 通过 Git submodule 固定到上游版本，本地源码保持独立 checkout：

- 来源：https://github.com/Nebuleon/ReGBA.git
- 固定提交：`7f379052dc14366e622a96c7cf7fb6b5a7e918a3`
- 使用共享 GBA 核心、MIPS JIT 和上游开放替代 BIOS；未接入 SDL/Linux 前端。

如果需要恢复该目录，在工作区根目录执行：

```powershell
git submodule update --init third_party/ReGBA
```

`scripts/prepare-regba.py` 核对提交及干净状态，再把需要的源文件复制到构建目录并应用有限适配；原 checkout 保持不变。修改详情在脚本及生成的 `regba-core/SOURCE.txt` 中可审查。构建目录不是需要手工维护的源目录。

原核心文件保留 GPL-2.0-or-later 版权声明；FreeRTOS、SDK 和 FatFs 保留各自声明。内嵌 BIOS 是上游 README 所述 Normmatt 开放替代 BIOS，SHA1 为 `598c0b2c6c5d15bbba218773574c9e7856d141f3`。自测 ROM 由本工程脚本生成，不包含商业游戏资源。分发固件时应同时提供对应源代码、适配脚本和相关许可声明。

中文 UI 字体从该固定版本的 `source/tools/unifont.hex.gz` 生成，版权和许可见上游 `doc/copyright_en.txt`，生成头文件不单独维护。完整依赖清单与许可说明见 [THIRD_PARTY_NOTICES.md](../THIRD_PARTY_NOTICES.md)。

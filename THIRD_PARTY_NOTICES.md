# Third-party sources and licenses

The X2600 FreeRTOS/ReGBA application and its adapted sources use
GPL-2.0-or-later where stated in their source headers. The root `LICENSE`
contains GPL version 2; existing component notices and later-version grants
remain in effect. This repository does not change upstream component licenses.

## Pinned source dependencies

| Git submodule | Upstream | Pinned commit | Use |
|---|---|---|---|
| `libbare-cpu` | <https://gitee.com/ingenic-dev/libbare-cpu> | `9500d2ec917a44d0d9657940e865e70f1a885849` | X2600 SDK, FreeRTOS kernel and peripheral support |
| `third_party/ReGBA` | <https://github.com/Nebuleon/ReGBA> | `7f379052dc14366e622a96c7cf7fb6b5a7e918a3` | GBA core, MIPS JIT, game compatibility data, open replacement BIOS and font source |
| `u-boot-ingenic` | `http://git1.ingenic.com.cn:8082/gerrit/bootloader/u-boot` | `501177955aab5f844d52a0a8ea3802e704f9ceca` | Optional historical bootloader build and DDR helper generation |

Initialize the two application dependencies with:

```sh
git submodule update --init libbare-cpu third_party/ReGBA
```

The U-Boot dependency is optional for the application build. Its existing local
changes are reproduced by `patches/halley7m-libbare-boot.patch` and
`patches/uboot-gcc7-elf-compat.patch`; `scripts/build-uboot.sh` applies them.
The submodule ignores working-tree dirtiness because applying these patches is
part of that build, but its pinned commit remains tracked by the parent repo.

## Component notices

- ReGBA core files retain their GPL-2.0-or-later notices. See
  `third_party/ReGBA/doc/copyright_en.txt` for the full upstream copyright and
  licensing inventory; some upstream artwork has separate licenses and is not
  used by this application's build.
- GNU Unifont is copyright 2000 Roman Czyborra, GPL-2.0-or-later, as recorded in
  that upstream inventory. `scripts/prepare-ui-font.py` generates the UI font
  from `third_party/ReGBA/source/tools/unifont.hex.gz`; generated font headers
  remain build products.
- The embedded replacement GBA BIOS comes from the pinned ReGBA source tree
  and is described by upstream as Normmatt's open replacement. It is not a
  Nintendo BIOS dump. See the upstream README and the checks in
  `scripts/prepare-regba.py` for its identity and provenance.
- FreeRTOS V10.5.1 in `libbare-cpu/Middlewares/FreeRTOS-Kernel` uses the MIT
  license supplied with that component. Preserve its license and source notices.
- X2600 SDK-derived files, the vendored FatFs files in
  `projects/halley7m-regba/platform/fatfs`, and adapted panel code retain the
  notices present in those files, including GPL-2.0-or-later notices.

Commercial game ROMs, Nintendo BIOS dumps, player saves, raw NOR/TF backups,
vendor toolchain/Cloner packages and original hardware reference packages are
not included. Tests generate their own diagnostic ROMs from the committed
scripts; source dependencies are retrieved from the pinned upstream repositories.

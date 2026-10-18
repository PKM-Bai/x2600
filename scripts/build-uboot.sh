#!/usr/bin/env bash
set -euo pipefail
workspace=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
toolchain_root=${1:-"$workspace/toolchains/xburst2-linux"}
source_dir="$workspace/u-boot-ingenic"
build_dir="$workspace/build/uboot-halley7m"
boot_patch="$workspace/patches/halley7m-libbare-boot.patch"
compat_patch="$workspace/patches/uboot-gcc7-elf-compat.patch"
target=x2600m_halley7_uImage_sfc_nor

if [[ ! -d "$toolchain_root" ]]; then
    echo 'XBurst2 toolchain missing. Run scripts/install-toolchain.sh with the official Linux archive.' >&2
    exit 1
fi
mapfile -t compilers < <(find "$toolchain_root" -type f -path '*/bin/mips*-gcc')
if [[ ${#compilers[@]} -ne 1 ]]; then
    echo "Expected one MIPS GCC in $toolchain_root; found ${#compilers[@]}." >&2
    exit 1
fi
compiler=${compilers[0]}
version=$("$compiler" --version)
if [[ "${version,,}" != *xburst2* ]]; then
    echo "An Ingenic XBurst2 toolchain is required. Detected: $version" >&2
    exit 1
fi
prefix=${compiler%gcc}
printf '%s\n' "$version"

if [[ ! -f "$source_dir/include/configs/x2600m_halley7.h" ]]; then
    echo 'Clone the official ingenic-master U-Boot into u-boot-ingenic first.' >&2
    exit 1
fi
for command in make gcc git; do
    command -v "$command" >/dev/null || { echo "Missing Linux host tool: $command" >&2; exit 1; }
done
gcc_version=$("$compiler" -dumpversion)
gcc_major=${gcc_version%%.*}
if [[ ! -f "$source_dir/include/linux/compiler-gcc$gcc_major.h" ]]; then
    echo "This U-Boot has no compiler-gcc$gcc_major.h. Use the official GCC 7 XBurst2 r5.1.6 toolchain." >&2
    exit 1
fi

payload="$workspace/build/linux-Release/halley7m_hello.bin"
if [[ -f "$payload" ]] && (( $(stat -c %s "$payload") > 0x600000 )); then
    echo 'The libbare payload exceeds the 6 MiB NOR application region.' >&2
    exit 1
fi
for patch in "$boot_patch" "$compat_patch"; do
    if git -C "$source_dir" apply --check "$patch" >/dev/null 2>&1; then
        git -C "$source_dir" apply "$patch"
    elif git -C "$source_dir" apply --reverse --check "$patch" >/dev/null 2>&1; then
        echo "Patch already applied: $(basename -- "$patch")"
    else
        echo "Patch does not match this U-Boot: $patch" >&2
        exit 1
    fi
done

mkdir -p -- "$build_dir"
if [[ ! -f "$build_dir/include/config.mk" ]]; then
    make -C "$source_dir" O="$build_dir" CROSS_COMPILE="$prefix" "${target}_config"
fi
build_log="$build_dir/build.log"
echo "Building U-Boot; log: $build_log"
# The vendor SPL recursively builds one CPU directory twice; parallel make races on .depend.
if ! make -C "$source_dir" O="$build_dir" CROSS_COMPILE="$prefix" --jobs=1 >"$build_log" 2>&1; then
    grep -nE 'error:|fatal error:|Error [0-9]+|No rule|undefined reference|not found|Stop\.|relocation .*against|error adding symbols' "$build_log" >&2 || tail -n 30 "$build_log" >&2
    exit 1
fi
image="$build_dir/u-boot-with-spl.bin"
test -s "$image"
if (( $(stat -c %s "$image") > 0x3f000 )); then
    echo 'U-Boot overlaps the NOR environment at 0x3f000. Review the flash layout before using this image.' >&2
    exit 1
fi
ls -lh "$image"
echo 'NOR layout: U-Boot at 0x0; libbare binary at 0x40000, maximum 0x600000 bytes.'
echo 'The boot command reads through 0x640000, so NOR capacity must be at least 6.25 MiB.'

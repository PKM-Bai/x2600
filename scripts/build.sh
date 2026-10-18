#!/usr/bin/env bash
set -euo pipefail
workspace=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
toolchain_root=${1:-"$workspace/toolchains/xburst2-linux"}
configuration=${2:-Release}
case "$configuration" in Debug|Release) ;; *) echo 'Configuration must be Debug or Release.' >&2; exit 1;; esac
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
printf '%s\n' "$version"
prefix=${compiler%gcc}
build_dir="$workspace/build/linux-$configuration"
cmake -S "$workspace/projects/halley7m-hello" -B "$build_dir" \
    -DCMAKE_TOOLCHAIN_FILE="$workspace/cmake/xburst2-elf.cmake" \
    -DCROSS_COMPILE="$prefix" -DCMAKE_BUILD_TYPE="$configuration"
cmake --build "$build_dir" --parallel "$(nproc)"
"${prefix}readelf" -h "$build_dir/halley7m_hello.elf"
ls -lh "$build_dir/halley7m_hello.bin"

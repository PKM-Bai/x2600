#!/usr/bin/env bash
set -euo pipefail
workspace=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
archive=$(realpath -- "${1:?Usage: bash scripts/install-toolchain.sh /path/to/mips-ingenic-xburst2-elf-tools-r5.1.6.tar.bz2}")
case "$(basename -- "$archive")" in mips-ingenic-xburst2-elf-tools-*.tar.bz2) ;; *) echo 'Select the official XBurst2 ELF Linux archive.' >&2; exit 1;; esac
destination="$workspace/toolchains/xburst2-linux"
if [[ -e "$destination" ]]; then
    echo "Destination exists: $destination. Keep the installed toolchain, or use a separate directory." >&2
    exit 1
fi
mkdir -p -- "$destination"
tar -xjf "$archive" -C "$destination"
find "$destination" -type f -path '*/bin/mips*-gcc'
echo 'Installed. Run bash scripts/build.sh to compile the HALLEY7-M example.'

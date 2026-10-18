#!/usr/bin/env bash
set -euo pipefail
workspace=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
source_dir="$workspace/u-boot-ingenic"
normal_build="$workspace/build/uboot-halley7m"
output_dir="$workspace/build/flash-probe"
creator_dir="$source_dir/tools/ingenic-tools/ddr_creator_x2000"

test -f "$normal_build/include/config.h"
command -v gcc >/dev/null
mkdir -p -- "$output_dir"
cat > "$output_dir/ddr-probe-config.h" <<'EOF'
#include <config.h>
#undef CONFIG_SYS_CPU_FREQ
#define CONFIG_SYS_CPU_FREQ 800000000
#undef CONFIG_SYS_APLL_FREQ
#define CONFIG_SYS_APLL_FREQ 800000000
#undef CONFIG_SYS_MPLL_FREQ
#define CONFIG_SYS_MPLL_FREQ 1200000000
#undef CONFIG_SYS_MEM_FREQ
#define CONFIG_SYS_MEM_FREQ 400000000
#undef CONFIG_X2600M_DDR2_MEM_FREQ
#define CONFIG_X2600M_DDR2_MEM_FREQ 400000000
#undef CONFIG_DDR_TYPE_DDR3
#undef CONFIG_X2600E_DDR3
#undef CONFIG_X2600H_DDR3L
#undef CONFIG_X2600N_DDR3
EOF

gcc -O2 -include "$source_dir/include/libfdt_env.h" \
    -include "$output_dir/ddr-probe-config.h" \
    -idirafter "$source_dir/include" \
    -idirafter "$normal_build/include2" \
    -idirafter "$normal_build/include" \
    -I "$source_dir/lib/libfdt" -I "$source_dir/tools" \
    -DUSE_HOSTCC -D__KERNEL_STRICT_NAMES \
    "$creator_dir/ddr_params_creator.c" \
    "$creator_dir/ddr2_params.c" \
    "$creator_dir/supported_ddr_chips.c" \
    -o "$output_dir/ddr-probe-creator"
"$output_dir/ddr-probe-creator" > "$output_dir/ddr_reg_values_400mhz.h"
python3 "$workspace/scripts/make-probe-ginfo.py"

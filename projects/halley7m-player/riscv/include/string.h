#ifndef PLAYER_RISCV_STRING_H
#define PLAYER_RISCV_STRING_H

/* The RISC-V image compiles SDK minimal/string.c, not Newlib's string ABI.
 * Its matching header supplies ulong/u8/u64 and omits the incompatible legacy
 * bcopy declaration. Keep this choice local to the RISC-V include directory. */
#include <stddef.h>
#include "../../../../libbare-cpu/lib/libc/minimal/include/string.h"

#endif

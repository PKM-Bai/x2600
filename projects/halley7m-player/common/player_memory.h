#ifndef PLAYER_MEMORY_H
#define PLAYER_MEMORY_H

#include <stdint.h>

/* One 64 MiB DDR device. Addresses ending in _PHYS are bus addresses. */
#define PLAYER_DDR_BYTES       UINT32_C(0x04000000)
#define PLAYER_MIPS_BASE       UINT32_C(0x80600000)
#define PLAYER_MIPS_END        UINT32_C(0x83c00000)
#define PLAYER_RISCV_PHYS      UINT32_C(0x03c00000)
#define PLAYER_RISCV_ENTRY     UINT32_C(0x03c00080)
#define PLAYER_RISCV_BYTES     UINT32_C(0x00200000)
#define PLAYER_SHARED_PHYS     UINT32_C(0x03e00000)
#define PLAYER_SHARED_UNCACHED UINT32_C(0xa3e00000)
#define PLAYER_SHARED_BYTES    UINT32_C(0x00200000)
#define PLAYER_PCM_OFFSET      UINT32_C(0x00010000)
#define PLAYER_PCM_BYTES       UINT32_C(0x00040000)
#define PLAYER_MAX_PACKAGE     UINT32_C(0x00600000)
#define PLAYER_MEMORY_VERSION UINT32_C(2)

_Static_assert(PLAYER_RISCV_PHYS + PLAYER_RISCV_BYTES == PLAYER_SHARED_PHYS,
               "Private and shared memory must not overlap");
_Static_assert(PLAYER_SHARED_PHYS + PLAYER_SHARED_BYTES == PLAYER_DDR_BYTES,
               "AMP memory must fit this 64 MiB board");
_Static_assert(PLAYER_PCM_OFFSET + PLAYER_PCM_BYTES <= PLAYER_SHARED_BYTES,
               "PCM ring must fit shared memory");

#endif

#ifndef PLAYER_MIPS_CONTEXT_H
#define PLAYER_MIPS_CONTEXT_H

/* pt_regs (184) + 32 64-bit FP slots + FCSR + padding = 448 bytes. */
#define PLAYER_FP_OFFSET 184
#define PLAYER_FCSR_OFFSET 440
#define PLAYER_FP_AREA_SIZE 264
#define PLAYER_CONTEXT_SIZE 448

#endif

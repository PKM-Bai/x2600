#ifndef REGBA_MIPS_CONTEXT_H
#define REGBA_MIPS_CONTEXT_H

/* pt_regs (184) + 32 64-bit FP slots + FCSR + padding = 448 bytes. */
#define REGBA_FP_OFFSET 184
#define REGBA_FCSR_OFFSET 440
#define REGBA_FP_AREA_SIZE 264
#define REGBA_CONTEXT_SIZE 448

#endif

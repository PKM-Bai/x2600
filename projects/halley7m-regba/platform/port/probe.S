/* Device-only register preservation probe. No peripherals are touched here.
 * Different seeds in two CPU-bound tasks detect corruption across tick IRQs. */
#include <asm/regdef.h>

    .set mips32r2
    .set noreorder
    .set noat
    .set hardfloat
    .text
    .globl platform_context_probe
    .ent platform_context_probe
platform_context_probe:
    addiu sp, sp, -144
    sw s0, 16(sp)
    sw s1, 20(sp)
    sw s2, 24(sp)
    sw s3, 28(sp)
    sw s4, 32(sp)
    sw s5, 36(sp)
    sw s6, 40(sp)
    sw s7, 44(sp)
    sw fp, 48(sp)
    sw gp, 52(sp)
    sw ra, 56(sp)
    sw a0, 60(sp)
    sw a2, 116(sp)
    sw a1, 120(sp)
    sdc1 $f20, 64(sp)
    sdc1 $f22, 72(sp)
    sdc1 $f24, 80(sp)
    sdc1 $f26, 88(sp)
    sdc1 $f28, 96(sp)
    sdc1 $f30, 104(sp)
    cfc1 v0, $31
    sw v0, 112(sp)

    /* Diagnose the FCSR write/read contract without a possible task switch.
     * This runs before gp and the probe registers receive their test seeds. */
    jal platform_context_fcsr_diagnostic
    nop
    lw a0, 60(sp)
    lw a1, 120(sp)

    .irp reg, $1,v1,a2,a3,t1,t2,t3,t4,t5,t6,t7,s0,s1,s2,s3,s4,s5,s6,s7,t8,t9,gp,fp,ra
    move \reg, a0
    .endr
    mthi a0
    nor v0, a0, zero
    mtlo v0
    /* XBurst2 reads fixed FCSR bits 18/19 as one. Snapshot the full hardware
     * readback atomically, then compare that entire word across preemption.
     * v0/t0 are probe scratch registers; a0 and every seeded register survive. */
    di t0
    ehb
    andi t0, t0, 1
    andi v0, a0, 3
    ctc1 v0, $31
    ehb
    cfc1 v0, $31
    ehb
    sw v0, 128(sp)
    beqz t0, 10f
    nop
    ei
    ehb
10:
    .irp reg, $f0,$f1,$f2,$f3,$f4,$f5,$f6,$f7,$f8,$f9,$f10,$f11,$f12,$f13,$f14,$f15,$f16,$f17,$f18,$f19,$f20,$f21,$f22,$f23,$f24,$f25,$f26,$f27,$f28,$f29,$f30,$f31
    mtc1 a0, \reg
    .endr

1:
    lw t0, 60(sp)
    .irp reg, $1,v1,a0,a2,a3,t1,t2,t3,t4,t5,t6,t7,s0,s1,s2,s3,s4,s5,s6,s7,t8,t9,gp,fp,ra
    bne \reg, t0, 2f
    nop
    .endr
    mfhi v0
    bne v0, t0, 3f
    nop
    mflo v0
    nor t0, t0, zero
    bne v0, t0, 4f
    nop
    nor t0, t0, zero
    .irp reg, $f0,$f1,$f2,$f3,$f4,$f5,$f6,$f7,$f8,$f9,$f10,$f11,$f12,$f13,$f14,$f15,$f16,$f17,$f18,$f19,$f20,$f21,$f22,$f23,$f24,$f25,$f26,$f27,$f28,$f29,$f30,$f31
    mfc1 v0, \reg
    bne v0, t0, 5f
    nop
    .endr
    cfc1 v0, $31
    lw t0, 128(sp)
    bne v0, t0, 6f
    nop
    /* Record A -> B -> A transitions while registers remain populated.
     * Serial execution alone cannot make both probes observe >1 transition. */
    lw t0, 116(sp)
    lw v0, 0(t0)
    lw t0, 0(v0)
    beq t0, a0, 8f
    nop
    sw a0, 0(v0)
    lw t0, 116(sp)
    lw v0, 4(t0)
    addiu v0, v0, 1
    sw v0, 4(t0)
8:
    addiu a1, a1, -1
    bnez a1, 1b
    nop
    b 7f
    move v0, zero
2:
    b 7f
    li v0, 1
3:
    b 7f
    li v0, 2
4:
    b 7f
    li v0, 3
5:
    b 7f
    li v0, 4
6:
    sw v0, 124(sp)
    li v0, 5
7:
    lw t0, 112(sp)
    ctc1 t0, $31
    ldc1 $f20, 64(sp)
    ldc1 $f22, 72(sp)
    ldc1 $f24, 80(sp)
    ldc1 $f26, 88(sp)
    ldc1 $f28, 96(sp)
    ldc1 $f30, 104(sp)
    lw s0, 16(sp)
    lw s1, 20(sp)
    lw s2, 24(sp)
    lw s3, 28(sp)
    lw s4, 32(sp)
    lw s5, 36(sp)
    lw s6, 40(sp)
    lw s7, 44(sp)
    lw fp, 48(sp)
    lw gp, 52(sp)
    li t0, 5
    bne v0, t0, 9f
    nop
    lw a0, 60(sp)
    lw a1, 124(sp)
    lw a2, 128(sp)
    jal platform_context_fcsr_failure
    nop
    li v0, 5
9:
    lw ra, 56(sp)
    jr ra
    addiu sp, sp, 144
    .end platform_context_probe

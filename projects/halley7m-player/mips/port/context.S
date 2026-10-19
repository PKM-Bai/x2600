/* Based on libbare-cpu's GPL-2.0-or-later MIPS exception/FreeRTOS entry. */
#include <asm/mipsregs.h>
#include <asm/regdef.h>
#include <asm/asm-offsets.h>
#include <asm/stackframe.h>
#include "context.h"

    .set mips32r2
    .set noreorder
    .set noat
    .text
    .globl player_exception_vector
    .globl player_exception_vector_end
player_exception_vector:
    /* Absolute target: this small stub is copied into each EBase vector slot. */
    lui k0, %hi(player_exception_entry)
    addiu k0, k0, %lo(player_exception_entry)
    jr k0
    nop
player_exception_vector_end:

player_exception_entry:
    addiu sp, sp, -PLAYER_FP_AREA_SIZE
    SAVE_ALL
    lw k0, PT_R29(sp)
    addiu k0, k0, PLAYER_FP_AREA_SIZE
    sw k0, PT_R29(sp)
    mfc0 k0, CP0_BADVADDR
    sw k0, PT_BVADDR(sp)
    /* Generated MIPS code may use gp as a general-purpose register. C IRQ
     * handlers must use the firmware small-data base, then restore task gp. */
    la gp, _gp
    /* FPU is saved before any C code can use it. No lazy ownership. */
    addiu a0, sp, PLAYER_FP_OFFSET
    addiu a1, sp, PLAYER_FCSR_OFFSET
    jal _save_fp_context
    nop
    move a0, sp
    la sp, __isr_stack_top
    /* o32 caller argument area, with room to keep the frame pointer. */
    addiu sp, sp, -24
    jal player_exception_dispatch
    nop
    /* C returns the selected task's full exception frame. */
    move sp, v0
    b player_restore_context
    nop

    .globl vPortISRStartFirstTask_S
    .ent vPortISRStartFirstTask_S
vPortISRStartFirstTask_S:
    la k0, pxCurrentTCB
    lw k1, 0(k0)
    lw sp, 0(k1)
player_restore_context:
    addiu a0, sp, PLAYER_FP_OFFSET
    addiu a1, sp, PLAYER_FCSR_OFFSET
    jal _restore_fp_context
    nop
    RESTORE_ALL_AND_RET
    .end vPortISRStartFirstTask_S

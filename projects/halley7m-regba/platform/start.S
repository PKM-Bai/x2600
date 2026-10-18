/* Standalone entry: abandon the U-Boot stack before touching application RAM. */
#include <asm/mipsregs.h>
#include <asm/regdef.h>

    .set mips32r2
    .set noreorder
    .section .start_section,"ax",@progbits
    .globl _start
    .ent _start
_start:
    di
    ehb
    /* All CP0 interrupt masks off; kernel, FR=0 hard-float o32, CU0/CU1 on. */
    li k0, ST0_CU0 | ST0_CU1
    mtc0 k0, CP0_STATUS
    ehb
    la sp, __boot_stack_top
    la gp, _gp
    move fp, zero
    la k0, __bss_start
    la k1, __bss_end
1:
    beq k0, k1, 2f
    nop
    sw zero, 0(k0)
    addiu k0, k0, 4
    b 1b
    nop
2:
    ctc1 zero, $31
    jal platform_c_entry
    nop
3:
    b 3b
    nop
    .end _start

    .text
    .globl mips_ihb
    .ent mips_ihb
mips_ihb:
    jr.hb ra
    nop
    .end mips_ihb

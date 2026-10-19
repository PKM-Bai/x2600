    .section .text.start,"ax",@progbits
    .globl reset_handler
reset_handler:
    csrwi mie, 0
    csrci mstatus, 8
    /* Victory0 SDK uses fence.i for its full cache synchronization operation. */
    fence.i
    .option push
    .option norelax
    la gp, __global_pointer$
    .option pop
    la sp, _stack_start
    la t0, __MTVEC
    csrw mtvec, t0
    la t0, _bss_start
    la t1, _bss_end
1:
    bgeu t0, t1, 2f
    sw zero, 0(t0)
    addi t0, t0, 4
    j 1b
2:
    call riscv_c_entry
3:
    wfi
    j 3b

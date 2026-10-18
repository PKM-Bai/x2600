/* ReGBA's patch handlers require clobbers limited to $1/$2/$4/$5.
 * XBurst2 KSEG0 publish order: L1 D writeback, L2 writeback, L1 I invalidate.
 * No C call: guest GP, HI/LO and all other guest registers remain intact.
 */
.set mips32r2
.set noat
.set noreorder
.text
.align 4
.globl ReGBA_MakeCodeVisible
.ent ReGBA_MakeCodeVisible
ReGBA_MakeCodeVisible:
    beq $5, $0, 9f
    nop
    addiu $sp, $sp, -16
    sw $4, 0($sp)
    addu $2, $4, $5
    sw $2, 4($sp)
    addiu $1, $0, -32
    and $4, $4, $1
1:
    cache 0x15, 0($4)
    addiu $4, $4, 32
    sltu $5, $4, $2
    bne $5, $0, 1b
    nop
    sync
    lw $4, 0($sp)
    addiu $1, $0, -64
    and $4, $4, $1
2:
    cache 0x17, 0($4)
    addiu $4, $4, 64
    sltu $5, $4, $2
    bne $5, $0, 2b
    nop
    sync
    lw $4, 0($sp)
    addiu $1, $0, -32
    and $4, $4, $1
3:
    cache 0x10, 0($4)
    addiu $4, $4, 32
    sltu $5, $4, $2
    bne $5, $0, 3b
    nop
    sync
    addiu $sp, $sp, 16
9:
    jr.hb $ra
    nop
.end ReGBA_MakeCodeVisible

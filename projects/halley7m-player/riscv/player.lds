OUTPUT_ARCH(riscv)
ENTRY(_reset_entry)
MEMORY { DDR (rwx) : ORIGIN = 0x03c00000, LENGTH = 2M }
SECTIONS
{
    .vectors : {
        __image_start = .;
        _reset_entry = . + 0x80;
        KEEP(*(.vectors))
    } > DDR
    .text : { *(.text.start) *(.text*) } > DDR
    .rodata : { . = ALIGN(16); *(.rodata*) *(.srodata*) } > DDR
    .data : {
        . = ALIGN(16);
        __global_pointer$ = . + 0x800;
        *(.data*) *(.sdata*)
        *(.interrupt_handler*)
    } > DDR
    . = ALIGN(16);
    __image_end = .;
    .bss (NOLOAD) : {
        . = ALIGN(16); _bss_start = .;
        *(.bss*) *(.sbss*) *(COMMON)
        . = ALIGN(16); _bss_end = .;
    } > DDR
    .stacks (NOLOAD) : {
        . = ALIGN(16); __boot_stack_start = .;
        . += 16384; _stack_start = .;
    } > DDR
    __rsc_table_start = 0;
    __rsc_table_end = 0;
    __private_end = ORIGIN(DDR) + LENGTH(DDR);
    ASSERT(_stack_start <= __private_end, "RISC-V static data/heap/stack exceed 2 MiB")
    ASSERT(_reset_entry == 0x03c00080, "RISC-V entry changed")
    /DISCARD/ : { *(.comment) *(.note*) *(.eh_frame*) }
}

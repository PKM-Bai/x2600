OUTPUT_ARCH(mips)
ENTRY(_start)

/* This is a standalone U-Boot go application. It never returns to U-Boot. */
MEMORY { DDR (rwx) : ORIGIN = 0x80600000, LENGTH = 54M }

SECTIONS
{
    .text : {
        __start = .;
        KEEP(*(.start_section))
        *(.text*)
        *(.interrupt_handler.text*)
    } > DDR
    .rodata : {
        . = ALIGN(8);
        /* Retain the verified companion even when AudioOwner=mips holds it reset. */
        KEEP(*(.rodata.riscv_image))
        *(.rodata*)
    } > DDR
    .data : { . = ALIGN(8); *(.data*) } > DDR
    .sdata : {
        . = ALIGN(16); _gp = . + 0x7ff0;
        *(.sdata*) *(.lit4) *(.lit8)
    } > DDR
    .got : { *(.got*) } > DDR
    .preinit_array : {
        . = ALIGN(4); __preinit_array_start = .;
        KEEP(*(.preinit_array)) __preinit_array_end = .;
    } > DDR
    .init_array : {
        . = ALIGN(4); __init_array_start = .;
        KEEP(*(SORT(.init_array.*))) KEEP(*(.init_array)) __init_array_end = .;
    } > DDR
    .fini_array : {
        . = ALIGN(4); __fini_array_start = .;
        KEEP(*(SORT(.fini_array.*))) KEEP(*(.fini_array)) __fini_array_end = .;
    } > DDR
    . = ALIGN(8);
    __image_copy_end = .;
    ASSERT(__image_copy_end - __start <= 6M, "Application exceeds 6 MiB RAM upload window")

    .bss (NOLOAD) : {
        . = ALIGN(16); __bss_start = .;
        *(.bss*) *(.sbss*) *(COMMON) *(.interrupt_handler.data*)
        . = ALIGN(16); __bss_end = .;
    } > DDR
    .vectors (NOLOAD) : {
        . = ALIGN(4096); __exception_base = .;
        . += 4096;
    } > DDR
    .stacks (NOLOAD) : {
        . = ALIGN(16); __boot_stack_start = .;
        . += 16384; __boot_stack_top = .;
        __isr_stack_start = .;
        . += 16384; __isr_stack_top = .;
    } > DDR
    .heap ALIGN(32) (NOLOAD) : {
        __heap_start = .;
        _user_heap_start = .;
    } > DDR
    __heap_end = ORIGIN(DDR) + LENGTH(DDR);
    _user_heap_end = __heap_end;
    ASSERT(__heap_start < __heap_end, "Static memory leaves no application heap")
    ASSERT(__heap_end - __heap_start >= 1M, "Need room for player runtime")
    /DISCARD/ : { *(.comment) *(.note*) *(.eh_frame*) *(.pdr) *(.MIPS.abiflags) *(.reginfo) }
}

main_stack_size = 16384;
exception_stack_size = 2048;
exception_section_size = 1024;
sram_start = 0x80600000;
sram_size = 16777216;
MEMORY { .sram : ORIGIN = sram_start, LENGTH = sram_size }

OUTPUT_ARCH(mips)
ENTRY(_start)

SECTIONS
{
 .text :
 {
  __start = .;
  *(.start_section*)
  __text_start = .;
  *(.text*)
  __text_end = .;
 } >.sram

 .rodata : {
  . = ALIGN(4);
  *(SORT_BY_ALIGNMENT(.rodata*))
 } >.sram

 .data : {
  . = ALIGN(4);
  *(SORT_BY_ALIGNMENT(.data*))
 } >.sram

 .sdata : {
  . = ALIGN(4);
  *(SORT_BY_ALIGNMENT(.sdata*))
 } >.sram

 .preinit_array  : {
   . = ALIGN(4);
   __preinit_array_start = .;
   KEEP (*(.preinit_array))
   __preinit_array_end = .;
 } >.sram

  .init_array     :
  {
    . = ALIGN(4);
    PROVIDE (__init_array_start = .);
    KEEP (*(SORT(.init_array.*)))
    KEEP (*(.init_array ))
    PROVIDE (__init_array_end = .);
  } >.sram

  .ctors          :
  {
    . = ALIGN(4);
    PROVIDE (__ctors_start = .);
    KEEP (*(EXCLUDE_FILE (*crtend.o *crtend?.o ) .ctors))
    KEEP (*(SORT(.ctors.*)))
    KEEP (*(.ctors))
    PROVIDE (__ctors_end = .);
  } >.sram

 .fini_array     : {
   __fini_array_start = .;
   *(.fini_array)
   __fini_array_end = .;
 } >.sram

 __ksymtab_strings :
 {
   KEEP(*(SORT(__ksymtab_strings*)))
 } >.sram

 ___ksymtab :
 {
   . = ALIGN(8);
   __ksymtab_start = .;
   KEEP(*(SORT(___ksymtab*)))
   __ksymtab_end = .;
 } >.sram

 .func_symbol_index :
 {
   . = ALIGN(16);
   __func_symbol_index_start = .;
   KEEP(*(SORT(.func_symbol_index*)))
   __func_symbol_index_end = .;
 } >.sram

  .func_symbol_str :
 {
   __func_symbol_str_start = .;
   KEEP(*(SORT(.func_symbol_str*)))
   __func_symbol_str_end = .;
 } >.sram

  /* module param */
  __param : {
  . = ALIGN(4);
  __param_start = .;
  KEEP(*(__param))
  __param_stop = .;
  } >.sram

 . = ALIGN(4);
 __image_copy_end = .;

 .bss : {
  . = ALIGN(4);
  __bss_start = .;
  *(.sbss.*)
  *(.bss.*)
  *(COMMON)
  . = ALIGN(4);
 } >.sram

 .bss_end : {
  . = ALIGN(4);
  __bss_end = .;
 } >.sram

 .exception_section : {
   . = ALIGN(4096);
   __exception_section_start = .;
   . = . + exception_section_size;
   KEEP(*(.exception_section))
   __exception_section_end = .;
 } >.sram

 .user_stack : {
	. = ALIGN(8);
    _user_stack_start = .;
    . = . + main_stack_size;
    KEEP(*(.user_stack))
    _user_stack_end = .;
  } >.sram

  .irq_stack : {
	. = ALIGN(8);
    _irq_stack_start = .;
    . = . + exception_stack_size;
    KEEP(*(.irq_stack))
    _irq_stack_end = .;
  } >.sram


 .exception_stack : {
	. = ALIGN(8);
    _exception_stack_start = .;
    . = . + exception_stack_size;
    KEEP(*(.exception_stack))
    _exception_stack_end = .;
  } >.sram


  /* must be last */
  .user_heap : {
    . =  ALIGN(8) ;
    _user_heap_start = .;
    KEEP(*(.user_heap))
    . = __start + sram_size;
    _user_heap_end = .;
    end = .;
  } >.sram

 /DISCARD/ : {
  *(.dynbss)
  *(.dynstr)
  *(.dynamic)
  *(.interp)
  *(.hash)
  *(.gnu.*)
  *(.plt)
  *(.got.plt)
  *(.rel.plt)
  *(.rel.dyn)
 }
}

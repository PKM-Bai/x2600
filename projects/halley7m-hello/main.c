#include <x2600_hal.h>

/* U-Boot must initialize DDR and UART before jumping to 0x80600000. */
int main(void)
{
    prom_printk("\nHALLEY7-M / X2600M libbare-cpu hello world\n");
    prom_printk("MIPS main core; DDR2 64MB; NOR boot\n");
    prom_printk("CP0 status=0x%x cause=0x%x\n",
                read_c0_status(), read_c0_cause());

    while (1) {
        asm volatile("nop");
    }
    return 0;
}

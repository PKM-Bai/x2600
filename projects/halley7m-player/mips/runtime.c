#include "platform_runtime.h"
#include "FreeRTOS.h"
#include "task.h"
#include <x2600_hal.h>
#include <x2600_ll_wdt.h>
#include <asm/ptrace.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

extern char __exception_base[];
extern unsigned char __isr_stack_start[], __isr_stack_top[];
extern char player_exception_vector[], player_exception_vector_end[];
extern void * volatile pxCurrentTCB;
extern void *player_port_dispatch(struct pt_regs *frame);
extern void __libc_init_array(void);
extern int main(void);

static volatile unsigned irq_depth;
static int initialized;

static void uart_init(void)
{
    CPM_GATE_Enable(CPM_Instance, CPM_CLKID_UART0);
    LL_GPIO_setPinMode(PE_Instance, 9, GPIO_MODE_FUNCTION1);
    LL_GPIO_setPinMode(PE_Instance, 10, GPIO_MODE_FUNCTION1);
    UART0_Instance->ULCR = 0x80;
    UART0_Instance->UDLHR = 0;
    UART0_Instance->UDLLR = 13;
    UART0_Instance->ULCR = 3;
    UART0_Instance->UIER = 0;
    UART0_Instance->UMR = 16;
    UART0_Instance->UACR = 0;
    UART0_Instance->UMCR = 0;
    UART0_Instance->UFCR = UFCR_UME | UFCR_FME | UFCR_RFRT | UFCR_TFRT;
    UART0_Instance->USPR = 0xa9;
}

void prom_putchar(const char c)
{
    while (!(UART0_Instance->ULSR & ULSR_TDRQ))
        __asm__ volatile("nop");
    UART0_Instance->UTHR = (unsigned char)c;
}

void prom_putstr(const char *s)
{
    while (*s) {
        if (*s == '\n') prom_putchar('\r');
        prom_putchar(*s++);
    }
}

int platform_uart_getchar(void)
{
    if (!(UART0_Instance->ULSR & ULSR_DRY)) return -1;
    return UART0_Instance->URBR & 0xff;
}

char prom_getchar(void)
{
    int c;
    do { c = platform_uart_getchar(); } while (c < 0);
    return (char)c;
}

void prom_printk(const char *format, ...)
{
    char buffer[512];
    va_list ap;
    va_start(ap, format);
    vsnprintf(buffer, sizeof(buffer), format, ap);
    va_end(ap);
    prom_putstr(buffer);
}

void prom_panic(const char *format, ...)
{
    char buffer[512];
    va_list ap;
    local_irq_disable();
    va_start(ap, format);
    vsnprintf(buffer, sizeof(buffer), format, ap);
    va_end(ap);
    platform_panic(buffer);
}

void platform_panic(const char *message)
{
    local_irq_disable();
    prom_putstr("\nPANIC: ");
    prom_putstr(message);
    prom_putstr("\n");
    for (;;) __asm__ volatile("nop");
}

void platform_reboot(void)
{
    prom_putstr("PLAYER_REBOOT watchdog; RAM diagnostics end, no NOR write\n");
    local_irq_disable();
    CPM_GATE_Enable(CPM_Instance, CPM_CLKID_TCU0);
    WDT0_Instance->WDT_ENABLE = 0;
    WDT0_Instance->TSCR = 1u << 16;
    WDT0_Instance->TMSR = (1u << 24) | (1u << 25);
    WDT0_Instance->WDT_FULL = 2048;
    WDT0_Instance->WDT_CONTROL = WDT_CONTROL_RTC_EN | WDT_CONTROL_CLRZ;
    WDT0_Instance->WDT_ENABLE = 1;
    for (;;) __asm__ volatile("nop");
}

void platform_assert_failed(const char *file, unsigned line)
{
    local_irq_disable();
    prom_printk("Assert: %s:%u\n", file, line);
    platform_panic("assertion failed");
}

void assert_failed(uint8_t *file, uint32_t line)
{
    platform_assert_failed((const char *)file, line);
}

unsigned platform_irq_depth(void) { return irq_depth; }
int in_interrupt(void) { return irq_depth != 0; }

void enable_irq_cpu(int irq) { set_c0_status(0x100UL << irq); }
void disable_irq_cpu(int irq) { clear_c0_status(0x100UL << irq); }

void *player_exception_dispatch(struct pt_regs *frame)
{
    ++irq_depth;
    if (*(uint32_t *)__isr_stack_start != 0xeeeeeeeeUL)
        platform_panic("ISR stack overflow");
    if ((frame->cp0_cause & CAUSEF_EXCCODE) || !pxCurrentTCB) {
        prom_printk("Exception cause=%08lx status=%08lx epc=%08lx badva=%08lx sp=%08lx hi=%08lx lo=%08lx\n",
                    frame->cp0_cause, frame->cp0_status, frame->cp0_epc,
                    frame->cp0_badvaddr, frame->regs[29], frame->hi, frame->lo);
        platform_panic("unhandled CPU exception");
    }
    void *next = player_port_dispatch(frame);
    --irq_depth;
    return next;
}

static void install_vectors(void)
{
    size_t length = player_exception_vector_end - player_exception_vector;
    memset(__exception_base, 0, 4096);
    const unsigned offsets[] = { 0, 0x80, 0x180, 0x200 };
    for (unsigned i = 0; i < sizeof(offsets)/sizeof(offsets[0]); ++i)
        memcpy(__exception_base + offsets[i], player_exception_vector, length);
    /* Publish copied instructions in D -> L2 -> I order. */
    CleanDCache_by_Addr((unsigned long *)__exception_base, 4096);
    flush_icache_range((unsigned long)__exception_base, (unsigned long)__exception_base + 4096);
    __asm__ volatile("sync\n\tehb" ::: "memory");
    write_c0_ebase((unsigned long)__exception_base);
    clear_c0_cause(CAUSEF_IV | CAUSEF_IP0 | CAUSEF_IP1);
    set_c0_cause(CAUSEF_DC);
    __asm__ volatile("ehb" ::: "memory");
}

void platform_init(void)
{
    if (initialized) return;
    local_irq_disable();
    /* Block U-Boot interrupt sources before installing application handlers. */
    INTC0_Instance->ICMSR0 = 0xffffffffUL;
    INTC0_Instance->ICMSR1 = 0xffffffffUL;
    INTC1_Instance->ICMSR0 = 0xffffffffUL;
    INTC1_Instance->ICMSR1 = 0xffffffffUL;
    CPM_GATE_Enable(CPM_Instance, CPM_CLKID_OST);
    CORE_OST0_Instance->OSTER = 0;
    CORE_OST0_Instance->OSTMR = OSTMR_FMASK;
    CORE_OST0_Instance->OSTFR = 0;
    uart_init();
    memset(__isr_stack_start, 0xee, __isr_stack_top - __isr_stack_start);
    install_vectors();
    /* irq_init briefly enables IE; peripheral masks above make this safe. */
    irq_init();
    local_irq_disable();
    LL_OST_GLOBAL_InitTypeDef init;
    LL_OST_GLOBAL_StructInit(&init);
    GLOBAL_OST_Instance->OSTER = 0;
    LL_OST_GLOBAL_Init(GLOBAL_OST_Instance, &init);
    initialized = 1;
}

static uint64_t global_cycles(void)
{
    /* Reading LOW snapshots HIGH into CNTB (also used by X2600 U-Boot).
     * Mask IRQs so another task/ISR cannot replace that snapshot between reads. */
    unsigned long flags = _local_irq_save();
    uint32_t low = GLOBAL_OST_Instance->OSTCNTL;
    uint32_t high = GLOBAL_OST_Instance->OSTCNTB;
    local_irq_restore(flags);
    return ((uint64_t)high << 32) | low;
}

uint64_t platform_time_us(void) { return global_cycles() / CYCLES_PER_US; }
size_t platform_isr_stack_unused(void)
{
    unsigned char *p = __isr_stack_start;
    while (p < __isr_stack_top && *p == 0xee) ++p;
    return (size_t)(p - __isr_stack_start);
}
uint64_t systick_get_time_us(void) { return platform_time_us(); }
HAL_StatusTypeDef HAL_InitTick(void) { platform_init(); return HAL_OK; }
HAL_StatusTypeDef HAL_Init(void) { platform_init(); return HAL_OK; }
HAL_StatusTypeDef HAL_DeInit(void) { return HAL_OK; }
uint64_t HAL_GetTick_Cycle(void) { return global_cycles(); }
uint64_t HAL_GetTick_Us(void) { return platform_time_us(); }
uint64_t HAL_GetTick_Ms(void) { return platform_time_us() / 1000; }
uint64_t HAL_GetTick(void) { return HAL_GetTick_Ms(); }

void HAL_uDelay(uint32_t us)
{
    uint64_t start = platform_time_us();
    while (platform_time_us() - start < us) __asm__ volatile("nop");
}

void HAL_Delay(uint32_t ms)
{
    if (!in_interrupt() && xTaskGetSchedulerState() == taskSCHEDULER_RUNNING && ms) {
        vTaskDelay(pdMS_TO_TICKS(ms) + 1);
    } else {
        uint64_t start = platform_time_us();
        while (platform_time_us() - start < (uint64_t)ms * 1000) __asm__ volatile("nop");
    }
}

void _init(void) { }
void _fini(void) { }
void platform_c_entry(void)
{
    platform_init();
    __libc_init_array();
    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);
    (void)main();
    platform_panic("main returned; U-Boot return is unsupported");
}

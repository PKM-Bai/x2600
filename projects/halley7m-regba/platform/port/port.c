/* FreeRTOS V10.5.1 XBurst2 main-core port, derived from the SDK MIPS port.
 * The upstream FreeRTOS port portions retain their MIT license. */
#include "FreeRTOS.h"
#include "task.h"
#include "platform_runtime.h"
#include "context.h"
#include <asm/ptrace.h>
#include <asm/asm-offsets.h>
#include <asm/mipsregs.h>
#include <x2600_hal.h>
#include <string.h>

struct regba_context {
    struct pt_regs gp;
    uint64_t fp[32];
    uint32_t fcsr;
    uint32_t pad;
} __attribute__((aligned(8)));

_Static_assert(sizeof(struct pt_regs) == PT_SIZE, "MIPS register offsets changed");
_Static_assert(sizeof(struct regba_context) == REGBA_CONTEXT_SIZE, "MIPS frame size changed");
_Static_assert(offsetof(struct regba_context, fp) == REGBA_FP_OFFSET, "FPU offset changed");
_Static_assert(offsetof(struct regba_context, fcsr) == REGBA_FCSR_OFFSET, "FCSR offset changed");

extern void * volatile pxCurrentTCB;
extern void vPortISRStartFirstTask_S(void);
static unsigned critical_depth;
static unsigned long critical_flags;
static volatile int switch_pending;

/* The two register-probe seeds differ in bit zero. Print one baseline per
 * seed, before the probe changes gp. Full readback is intentional: do not
 * assume the hardware implements every FCSR bit as writable. */
void platform_context_fcsr_diagnostic(uint32_t seed)
{
    static unsigned diagnosed;
    unsigned bit = 1U << (seed & 1U);
    uint32_t original, readback[4];
    unsigned long flags = _local_irq_save();
    if (diagnosed & bit) {
        local_irq_restore(flags);
        return;
    }
    diagnosed |= bit;
    __asm__ volatile("cfc1 %0,$31" : "=r"(original));
    for (uint32_t requested = 0; requested < 4; ++requested) {
        __asm__ volatile("ctc1 %1,$31\n\tehb\n\tcfc1 %0,$31\n\tehb"
                         : "=r"(readback[requested]) : "r"(requested) : "memory");
    }
    __asm__ volatile("ctc1 %0,$31\n\tehb" :: "r"(original) : "memory");
    local_irq_restore(flags);
    vTaskSuspendAll();
    prom_printk("FCSR_BASELINE seed=%08lx initial=%08lx write0=%08lx write1=%08lx write2=%08lx write3=%08lx\n",
                (unsigned long)seed, (unsigned long)original,
                (unsigned long)readback[0], (unsigned long)readback[1],
                (unsigned long)readback[2], (unsigned long)readback[3]);
    (void)xTaskResumeAll();
}

void platform_context_fcsr_failure(uint32_t seed, uint32_t observed, uint32_t expected)
{
    vTaskSuspendAll();
    prom_printk("FCSR_MISMATCH seed=%08lx expected=%08lx observed=%08lx\n",
                (unsigned long)seed, (unsigned long)expected,
                (unsigned long)observed);
    (void)xTaskResumeAll();
}

static void task_entry(TaskFunction_t fn, void *arg)
{
    fn(arg);
    vTaskDelete(NULL);
    platform_panic("deleted task resumed");
}

StackType_t *pxPortInitialiseStack(StackType_t *top, TaskFunction_t fn, void *arg)
{
    uintptr_t initial_sp = ((uintptr_t)(top + 1) & ~(uintptr_t)7) - 16;
    struct regba_context *ctx = (void *)(initial_sp - sizeof(*ctx));
    uintptr_t gp;
    __asm__("move %0,$28" : "=r"(gp));
    memset(ctx, 0, sizeof(*ctx));
    ctx->gp.regs[4] = (uintptr_t)fn;
    ctx->gp.regs[5] = (uintptr_t)arg;
    ctx->gp.regs[28] = gp;
    ctx->gp.regs[29] = initial_sp;
    ctx->gp.cp0_status = ST0_CU0 | ST0_CU1 | ST0_IE | ST0_EXL;
    ctx->gp.cp0_epc = (uintptr_t)task_entry;
    return (StackType_t *)ctx;
}

void os_enter_critical(void)
{
    unsigned long flags = _local_irq_save();
    if (critical_depth++ == 0)
        critical_flags = flags;
}

void os_exit_critical(void)
{
    configASSERT(critical_depth != 0);
    if (--critical_depth == 0)
        local_irq_restore(critical_flags);
}

int vPortIsInsideInterrupt(void)
{
    return platform_irq_depth() != 0;
}

void vPortYield(void)
{
    /* In an ISR request one switch at exception exit. In a task raise IP0. */
    if (vPortIsInsideInterrupt()) {
        switch_pending = 1;
    } else {
        unsigned long flags = _local_irq_save();
        set_c0_cause(CAUSEF_IP0);
        local_irq_restore(flags);
    }
}

static void software_interrupt(int irq, void *data)
{
    clear_c0_cause(CAUSEF_IP0);
    switch_pending = 1;
}

static void tick_interrupt(int irq, void *data)
{
    LL_OST_CORE_Clear_TFR_FFLAG(CORE_OST0_Instance);
    if (xTaskIncrementTick() != pdFALSE)
        switch_pending = 1;
}

/* Called on the dedicated ISR stack, with IE blocked by EXL. */
void *regba_port_dispatch(struct pt_regs *frame)
{
    unsigned pending = (frame->cp0_cause & frame->cp0_status & ST0_IM) >> 8;
    *(void **)pxCurrentTCB = frame;
    switch_pending = 0;
    for (unsigned i = 0; i < 8; ++i) {
        if (pending & (1U << i))
            handle_irq(IRQ_CPU_BASE + i);
    }
    if (switch_pending)
        vTaskSwitchContext();
    return *(void **)pxCurrentTCB;
}

BaseType_t xPortStartScheduler(void)
{
    LL_OST_CORE_InitTypeDef init = { .ticks_per_second = configTICK_RATE_HZ };
    local_irq_disable();
    clear_c0_cause(CAUSEF_IP0 | CAUSEF_IP1);
    ll_request_irq(IRQ_CPU_SW0, software_interrupt, NULL);
    ll_request_irq(IRQ_CPU_OST, tick_interrupt, NULL);
    CPM_GATE_Enable(CPM_Instance, CPM_CLKID_OST);
    CORE_OST0_Instance->OSTER = 0;
    LL_OST_CORE_Clear_TFR_FFLAG(CORE_OST0_Instance);
    configASSERT(LL_OST_CORE_Init(CORE_OST0_Instance, &init) == SUCCESS);
    critical_depth = 0;
    vPortISRStartFirstTask_S();
    return pdFALSE;
}

void vPortEndScheduler(void)
{
    platform_panic("scheduler returned");
}

void vApplicationStackOverflowHook(TaskHandle_t task, char *name)
{
    prom_putstr("Stack overflow: ");
    prom_putstr(name ? name : "?");
    platform_panic("task stack overflow");
}

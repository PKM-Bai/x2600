#include "player_felix.h"
#include "vpu_ops.h"
#include "FreeRTOS.h"
#include "task.h"
#include "semphr.h"
#include <x2600_hal.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>

/* X2600 CPM chapter SRBC: bits 31:29 are reserved; FELIX reset is bit 28.
 * Gate is CLKGR1 bit 17, not the X2000 HAL's bit 4. VPU map is 0x13300000.
 * Internal per-slice reset/VDMA descriptors follow the vendor FELIX API and
 * require board verification. All register waits below have bounded exits. */
#define FELIX_RESET (1u << 28)
#define FELIX_END (1u << 0)
#define FELIX_ERROR ((1u << 2) | (1u << 3) | (1u << 7) | (31u << 10) | (1u << 16))
#define FELIX_INTERRUPTS (SCH_INTE_ACFGERR | SCH_INTE_TLBERR | SCH_INTE_BSERR | SCH_INTE_ENDF)
#define FELIX_EARB_STATUS 0x0cu
#define FELIX_EARB_BUSY (1u << 21)

struct player_felix_state {
    struct vpu_ctx *context;
    SemaphoreHandle_t completion;
    volatile int waiting;
};

static volatile uint32_t *felix_register(uint32_t offset)
{
    return (volatile uint32_t *)(uintptr_t)(VPU_BASE + offset);
}

static void cpm_reset(void)
{
    uint32_t value = (uint32_t)CPM_Instance->SRBC;
    CPM_Instance->SRBC = value | FELIX_RESET;
    __asm__ volatile("sync" ::: "memory");
    (void)CPM_Instance->SRBC;
    CPM_Instance->SRBC = value & ~FELIX_RESET;
    __asm__ volatile("sync" ::: "memory");
}

static void felix_irq(int irq, void *opaque)
{
    struct player_felix_state *state = opaque;
    struct vpu_ctx *context = state->context;
    uint32_t status = *felix_register(REG_SCH_STAT);
    BaseType_t wake = pdFALSE;
    (void)irq;
    context->sch_stat = status;
    context->sde_stat = *felix_register(REG_SDE_STAT);
    context->error = (!(status & FELIX_END) || (status & FELIX_ERROR)) ? 1u : 0u;
    *felix_register(REG_SDE_STAT) &= ~(1u << 1);
    *felix_register(0x70070) &= ~1u;
    *felix_register(REG_SCH_GLBC) &= ~FELIX_INTERRUPTS;
    __asm__ volatile("sync" ::: "memory");
    if (state->waiting) {
        state->waiting = 0;
        xSemaphoreGiveFromISR(state->completion, &wake);
    }
    portYIELD_FROM_ISR(wake);
}

static int felix_start(void *opaque)
{
    struct player_felix_state *state = opaque;
    struct vpu_ctx *context = state->context;
    TickType_t start, timeout = pdMS_TO_TICKS(10);
    if (!timeout) timeout = 1;
    ll_disable_irq(IRQ_INTC0_FELIX);
    while (xSemaphoreTake(state->completion, 0) == pdTRUE) { }
    context->error = 0; context->sch_stat = 0; context->sde_stat = 0;
    /* Vendor internal reset. CPM reset is used for session start/stop. */
    *felix_register(REG_SCH_GLBC) = 1u << 30;
    start = xTaskGetTickCount();
    while (*felix_register(FELIX_EARB_STATUS) & FELIX_EARB_BUSY) {
        if ((TickType_t)(xTaskGetTickCount() - start) >= timeout) {
            context->error = 1;
            cpm_reset();
            return -ETIMEDOUT;
        }
    }
    /* Descriptors, RBSP and DPB side data are all cached SDK allocations. */
    CleanDCache();
    state->waiting = 1;
    *felix_register(REG_SCH_GLBC) = SCH_GLBC_HIAXI | FELIX_INTERRUPTS;
    __asm__ volatile("sync" ::: "memory");
    ll_enable_irq(IRQ_INTC0_FELIX);
    *felix_register(REG_VDMA_TASKRG) = (context->desc_pa & 0xffffff80u) | 1u;
    __asm__ volatile("sync" ::: "memory");
    return 0;
}

static int felix_wait(void *opaque)
{
    struct player_felix_state *state = opaque;
    TickType_t timeout = pdMS_TO_TICKS(100);
    if (!timeout) timeout = 1;
    if (xSemaphoreTake(state->completion, timeout) != pdTRUE) {
        state->context->error = 1;
        (void)player_felix_stop(state->context);
        return -ETIMEDOUT;
    }
    return state->context->error ? -EIO : 0;
}

static int felix_end(void *opaque)
{
    (void)opaque;
    return 0;
}

static struct vpu_ops felix_ops = { felix_start, felix_wait, felix_end };

int vpu_ctx_init(struct vpu_ctx *context)
{
    struct player_felix_state *state = calloc(1, sizeof(*state));
    if (!state) return -ENOMEM;
    state->completion = xSemaphoreCreateBinary();
    if (!state->completion) { free(state); return -ENOMEM; }
    if (CPM_GATE_Enable(CPM_Instance, CPM_CLKID_FELIX) != HAL_OK) {
        vSemaphoreDelete(state->completion); free(state); return -EIO;
    }
    state->context = context;
    context->vpu_base = VPU_BASE; context->ops = &felix_ops; context->priv = state;
    cpm_reset();
    ll_request_irq(IRQ_INTC0_FELIX, felix_irq, state);
    ll_disable_irq(IRQ_INTC0_FELIX);
    return 0;
}

int player_felix_stop(struct vpu_ctx *context)
{
    struct player_felix_state *state = context ? context->priv : NULL;
    if (!state) return 0;
    ll_disable_irq(IRQ_INTC0_FELIX);
    state->waiting = 0;
    *felix_register(REG_SCH_GLBC) &= ~FELIX_INTERRUPTS;
    cpm_reset();
    return 0;
}

void vpu_ctx_uninit(struct vpu_ctx *context)
{
    struct player_felix_state *state = context->priv;
    if (!state) return;
    (void)player_felix_stop(context);
    ll_release_irq(IRQ_INTC0_FELIX);
    (void)CPM_GATE_Disable(CPM_Instance, CPM_CLKID_FELIX);
    vSemaphoreDelete(state->completion);
    free(state);
    context->priv = NULL;
}

int vpu_ctx_set_ops(struct vpu_ctx *context, void *opaque, struct vpu_ops *ops)
{
    context->priv = opaque; context->ops = ops;
    return 0;
}

int vpu_hw_start(struct vpu_ctx *context) { return context->ops->start(context->priv); }
int vpu_hw_wait(struct vpu_ctx *context) { return context->ops->wait(context->priv); }
int vpu_hw_end(struct vpu_ctx *context) { return context->ops->end(context->priv); }

#include "player_cpu_diagnostics.h"
#include "FreeRTOS.h"
#include "task.h"
#if defined(__riscv)
#include <x2600_hal.h>
#else
#include "platform_runtime.h"
extern char __heap_start[], __heap_end[];
#endif

uint64_t player_cpu_diagnostics_counter(void)
{
#if defined(__riscv)
    uint32_t high, low;
    do {
        high = RISC_CCU_Instance->TIME_H;
        low = RISC_CCU_Instance->TIME_L;
    } while (high != RISC_CCU_Instance->TIME_H);
    return ((uint64_t)high << 32) | low;
#else
    return platform_time_us();
#endif
}

int player_cpu_diagnostics_capture(player_cpu_raw *raw)
{
#if configGENERATE_RUN_TIME_STATS == 1 && INCLUDE_xTaskGetIdleTaskHandle == 1
    taskENTER_CRITICAL();
    raw->counter = player_cpu_diagnostics_counter();
    raw->idle_counter = ulTaskGetIdleRunTimeCounter();
    taskEXIT_CRITICAL();
    raw->heap_free_bytes = (uint32_t)xPortGetFreeHeapSize();
#if defined(__riscv)
    raw->counter_hz = (uint32_t)configCPU_CLOCK_HZ;
    raw->heap_total_bytes = (uint32_t)configTOTAL_HEAP_SIZE;
    raw->heap_min_free_bytes = (uint32_t)xPortGetMinimumEverFreeHeapSize();
    raw->heap_min_kind = PLAYER_HEAP_MIN_ALLOCATOR;
#else
    static uint32_t minimum_observed = UINT32_MAX;
    raw->counter_hz = UINT32_C(1000000);
    raw->heap_total_bytes = (uint32_t)(__heap_end - __heap_start);
    if (raw->heap_free_bytes < minimum_observed) minimum_observed = raw->heap_free_bytes;
    raw->heap_min_free_bytes = minimum_observed;
    raw->heap_min_kind = PLAYER_HEAP_MIN_SAMPLED;
#endif
    return 1;
#else
    (void)raw;
    return 0;
#endif
}

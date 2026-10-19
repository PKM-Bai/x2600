#ifndef PLAYER_RISCV_FREERTOS_CONFIG_H
#define PLAYER_RISCV_FREERTOS_CONFIG_H
#include <stdint.h>
#include <stddef.h>
#include <x2600_hal.h>
void player_riscv_assert(const char *file, unsigned line) __attribute__((noreturn));
#define configMTIME_BASE_ADDRESS        0x12a00010
#define configMTIMECMP_BASE_ADDRESS     0x12a00018
#define configCPU_CLOCK_HZ              PLAYER_RISCV_CLOCK_HZ
#define configTICK_RATE_HZ              1000
#define configUSE_PREEMPTION            1
#define configUSE_TIME_SLICING          1
#define configMAX_PRIORITIES            5
#define configMINIMAL_STACK_SIZE        256
#define configMAX_TASK_NAME_LEN         20
#define configTOTAL_HEAP_SIZE           (128 * 1024)
#define configISR_STACK_SIZE_WORDS      1024
#define configUSE_16_BIT_TICKS           0
#define configIDLE_SHOULD_YIELD          1
#define configUSE_MUTEXES               1
#define configUSE_RECURSIVE_MUTEXES      0
#define configUSE_COUNTING_SEMAPHORES    1
#define configUSE_TASK_NOTIFICATIONS     1
#define configUSE_PORT_OPTIMISED_TASK_SELECTION 1
#define configSUPPORT_DYNAMIC_ALLOCATION 1
#define configSUPPORT_STATIC_ALLOCATION 0
#define configCHECK_FOR_STACK_OVERFLOW   2
#define configUSE_MALLOC_FAILED_HOOK     1
#define configUSE_IDLE_HOOK              0
#define configUSE_TICK_HOOK              0
#define configUSE_TIMERS                 0
#define configUSE_CO_ROUTINES            0
#define configQUEUE_REGISTRY_SIZE        0
#define configGENERATE_RUN_TIME_STATS    0
#define INCLUDE_vTaskDelay               1
#define INCLUDE_vTaskDelete              1
#define INCLUDE_vTaskSuspend             1
#define INCLUDE_xTaskGetSchedulerState   1
#define INCLUDE_uxTaskGetStackHighWaterMark 1
#define configASSERT(x) do { if (!(x)) player_riscv_assert(__FILE__, __LINE__); } while (0)
#endif

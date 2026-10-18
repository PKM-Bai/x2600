#ifndef REGBA_FREERTOS_CONFIG_H
#define REGBA_FREERTOS_CONFIG_H

#include <stdint.h>
#include <stddef.h>
#include "platform_runtime.h"

#define configUSE_PREEMPTION                     1
#define configUSE_TIME_SLICING                   1
#define configCPU_CLOCK_HZ                       1152000000UL
#define configTICK_RATE_HZ                       1000UL
#define configMAX_PRIORITIES                     8
#define configMINIMAL_STACK_SIZE                 512
#define configMAX_TASK_NAME_LEN                  24
#define configUSE_16_BIT_TICKS                    0
#define configIDLE_SHOULD_YIELD                   1
#define configUSE_PORT_OPTIMISED_TASK_SELECTION   1
#define configUSE_NEWLIB_REENTRANT                1
#define configUSE_MUTEXES                        1
#define configUSE_RECURSIVE_MUTEXES              1
#define configUSE_COUNTING_SEMAPHORES            1
#define configUSE_TASK_NOTIFICATIONS             1
#define configQUEUE_REGISTRY_SIZE                0
#define configUSE_TRACE_FACILITY                 1
#define configUSE_STATS_FORMATTING_FUNCTIONS     0
#define configCHECK_FOR_STACK_OVERFLOW           2
#define configUSE_MALLOC_FAILED_HOOK              0
#define configUSE_IDLE_HOOK                       0
#define configUSE_TICK_HOOK                       0
#define configUSE_TICKLESS_IDLE                   0
#define configSUPPORT_DYNAMIC_ALLOCATION         1
#define configSUPPORT_STATIC_ALLOCATION          0
#define configUSE_TIMERS                         1
#define configTIMER_TASK_PRIORITY                2
#define configTIMER_QUEUE_LENGTH                 8
#define configTIMER_TASK_STACK_DEPTH             768
#define configUSE_CO_ROUTINES                    0
#define configUSE_APPLICATION_TASK_TAG           0
#define configGENERATE_RUN_TIME_STATS            1
#define configRUN_TIME_COUNTER_TYPE              uint64_t
/* platform_init() already starts the 64-bit global OST before scheduling. */
#define portCONFIGURE_TIMER_FOR_RUN_TIME_STATS() do { } while (0)
#define portGET_RUN_TIME_COUNTER_VALUE()         platform_time_us()
#define INCLUDE_vTaskPrioritySet                 1
#define INCLUDE_uxTaskPriorityGet                1
#define INCLUDE_vTaskDelete                      1
#define INCLUDE_vTaskSuspend                     1
#define INCLUDE_vTaskDelay                       1
#define INCLUDE_vTaskDelayUntil                  1
#define INCLUDE_xTaskGetCurrentTaskHandle        1
#define INCLUDE_xTaskGetSchedulerState           1
#define INCLUDE_uxTaskGetStackHighWaterMark       1
#define INCLUDE_xTaskGetIdleTaskHandle           1
#define INCLUDE_eTaskGetState                    1
#define configASSERT(x) do { if (!(x)) platform_assert_failed(__FILE__, __LINE__); } while (0)

#endif

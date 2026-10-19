#ifndef PLAYER_STORAGE_TEST_FREERTOS_H
#define PLAYER_STORAGE_TEST_FREERTOS_H
#include <stdint.h>
typedef void *TaskHandle_t;
typedef uint32_t TickType_t;
enum { taskSCHEDULER_NOT_STARTED, taskSCHEDULER_RUNNING };
int xPortIsInsideInterrupt(void);
int xTaskGetSchedulerState(void);
TaskHandle_t xTaskGetCurrentTaskHandle(void);
void vTaskDelay(TickType_t ticks);
#endif

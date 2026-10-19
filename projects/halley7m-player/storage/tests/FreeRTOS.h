#ifndef PLAYER_STORAGE_TEST_FREERTOS_H
#define PLAYER_STORAGE_TEST_FREERTOS_H
typedef void *TaskHandle_t;
enum { taskSCHEDULER_NOT_STARTED, taskSCHEDULER_RUNNING };
int xPortIsInsideInterrupt(void);
int xTaskGetSchedulerState(void);
TaskHandle_t xTaskGetCurrentTaskHandle(void);
#endif

#ifndef AI_ENTROPY_TEST_TASK_H
#define AI_ENTROPY_TEST_TASK_H
#include "FreeRTOS.h"
#define taskSCHEDULER_NOT_STARTED 0
#define taskSCHEDULER_RUNNING 2
BaseType_t xTaskGetSchedulerState(void);
#endif

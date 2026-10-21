#ifndef TOUCH_TEST_TASK_H
#define TOUCH_TEST_TASK_H
#include <stdint.h>
static inline void vTaskDelay(uint32_t ticks) { (void)ticks; }
#endif

#ifndef TOUCH_TEST_FREERTOS_H
#define TOUCH_TEST_FREERTOS_H
#include <stdint.h>
#define pdMS_TO_TICKS(value) (value)
#define taskENTER_CRITICAL() ((void)0)
#define taskEXIT_CRITICAL() ((void)0)
#endif

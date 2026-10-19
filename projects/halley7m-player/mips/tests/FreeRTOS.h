#ifndef PLAYER_SESSION_TEST_FREERTOS_H
#define PLAYER_SESSION_TEST_FREERTOS_H
#include <stdint.h>
typedef uint32_t TickType_t;
typedef void *TaskHandle_t;
#define pdMS_TO_TICKS(value) ((TickType_t)(value))
#define taskENTER_CRITICAL() do { } while (0)
#define taskEXIT_CRITICAL() do { } while (0)
#endif

#ifndef AI_ENTROPY_TEST_FREERTOS_H
#define AI_ENTROPY_TEST_FREERTOS_H
#include <assert.h>
#include <stdint.h>
typedef int BaseType_t;
typedef uint32_t TickType_t;
#define pdTRUE 1
#define pdFALSE 0
#define configTICK_RATE_HZ 1000u
#define configASSERT(value) assert(value)
#endif

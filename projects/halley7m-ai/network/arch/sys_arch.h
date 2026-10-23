#ifndef AI_LWIP_SYS_ARCH_H
#define AI_LWIP_SYS_ARCH_H

#include "FreeRTOS.h"
#include "task.h"
#include "queue.h"
#include "semphr.h"

typedef SemaphoreHandle_t sys_sem_t;
typedef SemaphoreHandle_t sys_mutex_t;
typedef QueueHandle_t sys_mbox_t;
typedef TaskHandle_t sys_thread_t;
#define SYS_SEM_NULL NULL
#define SYS_MBOX_NULL NULL

#endif

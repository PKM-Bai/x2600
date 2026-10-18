#include "platform_runtime.h"
#include "FreeRTOS.h"
#include "task.h"
#include <errno.h>
#include <malloc.h>
#include <reent.h>
#include <stdlib.h>
#include <stdint.h>

extern char __heap_start[], __heap_end[];
static char *heap_break = __heap_start;

void __malloc_lock(struct _reent *reent)
{
    configASSERT(!xPortIsInsideInterrupt());
    vTaskSuspendAll();
}

void __malloc_unlock(struct _reent *reent)
{
    (void)xTaskResumeAll();
}

void __env_lock(struct _reent *reent) { __malloc_lock(reent); }
void __env_unlock(struct _reent *reent) { __malloc_unlock(reent); }

void *_sbrk_r(struct _reent *reent, ptrdiff_t increment)
{
    void *result = (void *)-1;
    configASSERT(!xPortIsInsideInterrupt());
    vTaskSuspendAll();
    /* Do bounds arithmetic without forming an out-of-range C pointer. */
    if ((increment >= 0 && (uintptr_t)increment <= (uintptr_t)(__heap_end - heap_break)) ||
        (increment < 0 && (uintptr_t)(-(increment + 1)) + 1 <= (uintptr_t)(heap_break - __heap_start))) {
        result = heap_break;
        heap_break += increment;
    } else {
        reent->_errno = ENOMEM;
    }
    /* Every path, including ENOMEM, balances suspension. */
    (void)xTaskResumeAll();
    return result;
}

void *_sbrk(ptrdiff_t increment) { return _sbrk_r(_impure_ptr, increment); }

void *pvPortMalloc(size_t size) { return malloc(size); }
void vPortFree(void *ptr) { free(ptr); }
void vPortInitialiseBlocks(void) { }

size_t platform_free_heap(void)
{
    size_t result;
    configASSERT(!xPortIsInsideInterrupt());
    vTaskSuspendAll();
    struct mallinfo info = mallinfo();
    result = (size_t)(__heap_end - heap_break) + (size_t)info.fordblks;
    (void)xTaskResumeAll();
    return result;
}

size_t xPortGetFreeHeapSize(void) { return platform_free_heap(); }

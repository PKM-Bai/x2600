/* Native FreeRTOS port for lwIP 2.2.1. No CMSIS-OS dependency. */
#include "lwip/sys.h"
#include "ai_lwip_platform.h"
#include <limits.h>

static ai_lwip_random_source_t random_source;
static void *random_context;
static uint32_t utc_seconds;
static uint64_t utc_monotonic_us;

static TickType_t wait_ticks(u32_t timeout_ms)
{
    if (timeout_ms == 0)
        return portMAX_DELAY;
    uint64_t ticks = ((uint64_t)timeout_ms * configTICK_RATE_HZ + 999) / 1000;
    if (ticks >= portMAX_DELAY)
        return portMAX_DELAY - 1;
    return (TickType_t)ticks;
}

static u32_t elapsed_ms(TickType_t start)
{
    TickType_t elapsed = xTaskGetTickCount() - start;
    uint64_t ms = (uint64_t)elapsed * 1000 / configTICK_RATE_HZ;
    return ms >= SYS_ARCH_TIMEOUT ? SYS_ARCH_TIMEOUT - 1 : (u32_t)ms;
}

void sys_init(void) { }
u32_t sys_now(void)
{
    return (u32_t)((uint64_t)xTaskGetTickCount() * 1000 / configTICK_RATE_HZ);
}
u32_t sys_jiffies(void) { return (u32_t)xTaskGetTickCount(); }

err_t sys_sem_new(sys_sem_t *sem, u8_t count)
{
    configASSERT(count <= 1);
    *sem = xSemaphoreCreateBinary();
    if (*sem == NULL)
        return ERR_MEM;
    if (count) {
        BaseType_t result = xSemaphoreGive(*sem);
        configASSERT(result == pdTRUE);
        (void)result;
    }
    return ERR_OK;
}
void sys_sem_signal(sys_sem_t *sem)
{
    /* A binary semaphore may already hold a wakeup; coalescing is valid. */
    (void)xSemaphoreGive(*sem);
}
u32_t sys_arch_sem_wait(sys_sem_t *sem, u32_t timeout)
{
    TickType_t start = xTaskGetTickCount();
    return xSemaphoreTake(*sem, wait_ticks(timeout)) == pdTRUE
        ? elapsed_ms(start) : SYS_ARCH_TIMEOUT;
}
void sys_sem_free(sys_sem_t *sem) { vSemaphoreDelete(*sem); *sem = NULL; }
int sys_sem_valid(sys_sem_t *sem) { return sem && *sem != NULL; }
void sys_sem_set_invalid(sys_sem_t *sem) { *sem = NULL; }

err_t sys_mutex_new(sys_mutex_t *mutex)
{
    *mutex = xSemaphoreCreateMutex();
    return *mutex != NULL ? ERR_OK : ERR_MEM;
}
void sys_mutex_lock(sys_mutex_t *mutex)
{
    BaseType_t result = xSemaphoreTake(*mutex, portMAX_DELAY);
    configASSERT(result == pdTRUE);
    (void)result;
}
void sys_mutex_unlock(sys_mutex_t *mutex)
{
    BaseType_t result = xSemaphoreGive(*mutex);
    configASSERT(result == pdTRUE);
    (void)result;
}
void sys_mutex_free(sys_mutex_t *mutex) { vSemaphoreDelete(*mutex); *mutex = NULL; }
int sys_mutex_valid(sys_mutex_t *mutex) { return mutex && *mutex != NULL; }
void sys_mutex_set_invalid(sys_mutex_t *mutex) { *mutex = NULL; }

err_t sys_mbox_new(sys_mbox_t *mbox, int size)
{
    configASSERT(size > 0);
    *mbox = xQueueCreate((UBaseType_t)size, sizeof(void *));
    return *mbox != NULL ? ERR_OK : ERR_MEM;
}
void sys_mbox_post(sys_mbox_t *mbox, void *message)
{
    BaseType_t result = xQueueSend(*mbox, &message, portMAX_DELAY);
    configASSERT(result == pdTRUE);
    (void)result;
}
err_t sys_mbox_trypost(sys_mbox_t *mbox, void *message)
{
    return xQueueSend(*mbox, &message, 0) == pdTRUE ? ERR_OK : ERR_MEM;
}
err_t sys_mbox_trypost_fromisr(sys_mbox_t *mbox, void *message)
{
    BaseType_t higher_priority_woken = pdFALSE;
    BaseType_t result = xQueueSendFromISR(*mbox, &message, &higher_priority_woken);
    portYIELD_FROM_ISR(higher_priority_woken);
    return result == pdTRUE ? ERR_OK : ERR_MEM;
}
u32_t sys_arch_mbox_fetch(sys_mbox_t *mbox, void **message, u32_t timeout)
{
    void *discard;
    TickType_t start = xTaskGetTickCount();
    return xQueueReceive(*mbox, message ? message : &discard, wait_ticks(timeout)) == pdTRUE
        ? elapsed_ms(start) : SYS_ARCH_TIMEOUT;
}
u32_t sys_arch_mbox_tryfetch(sys_mbox_t *mbox, void **message)
{
    void *discard;
    return xQueueReceive(*mbox, message ? message : &discard, 0) == pdTRUE
        ? 0 : SYS_MBOX_EMPTY;
}
void sys_mbox_free(sys_mbox_t *mbox)
{
    configASSERT(uxQueueMessagesWaiting(*mbox) == 0);
    vQueueDelete(*mbox);
    *mbox = NULL;
}
int sys_mbox_valid(sys_mbox_t *mbox) { return mbox && *mbox != NULL; }
void sys_mbox_set_invalid(sys_mbox_t *mbox) { *mbox = NULL; }

sys_thread_t sys_thread_new(const char *name, lwip_thread_fn thread,
                           void *argument, int stack_bytes, int priority)
{
    TaskHandle_t task = NULL;
    configASSERT(stack_bytes > 0 && priority >= 0 && priority < configMAX_PRIORITIES);
    size_t words = ((size_t)stack_bytes + sizeof(StackType_t) - 1) / sizeof(StackType_t);
    configASSERT(words <= (configSTACK_DEPTH_TYPE)-1);
    if (xTaskCreate(thread, name, (configSTACK_DEPTH_TYPE)words, argument,
                    (UBaseType_t)priority, &task) != pdPASS)
        return NULL;
    return task;
}

sys_prot_t sys_arch_protect(void)
{
    return portSET_INTERRUPT_MASK_FROM_ISR();
}
void sys_arch_unprotect(sys_prot_t level)
{
    portCLEAR_INTERRUPT_MASK_FROM_ISR(level);
}

void ai_lwip_set_random_source(ai_lwip_random_source_t source, void *context)
{
    /* Called at startup before any lwIP task. */
    random_source = source;
    random_context = context;
}
bool ai_lwip_random_ready(void) { return random_source != NULL; }
uint32_t ai_lwip_random(void)
{
    uint32_t value;
    if (random_source == NULL || !random_source(random_context, &value, sizeof(value)))
        ai_lwip_assert("random source unavailable", __FILE__, __LINE__);
    return value;
}
void ai_lwip_assert(const char *message, const char *file, int line)
{
    (void)message;
    (void)file;
    (void)line;
    configASSERT(0);
    /* Failure must stay fatal even if a later build disables configASSERT. */
    for (;;) { }
}
void ai_lwip_set_unix_time(uint32_t seconds)
{
    /* Reject a reset/default clock. Firmware baseline: 2026-01-01 UTC. */
    if (seconds < UINT32_C(1767225600))
        return;
    taskENTER_CRITICAL();
    utc_seconds = seconds;
    utc_monotonic_us = platform_time_us();
    taskEXIT_CRITICAL();
}
bool ai_lwip_get_unix_time(uint64_t *seconds)
{
    taskENTER_CRITICAL();
    uint32_t base = utc_seconds;
    uint64_t elapsed_us = platform_time_us() - utc_monotonic_us;
    taskEXIT_CRITICAL();
    /* SNTP refresh is hourly. Expire stale samples before tick-wrap ambiguity. */
    if (!base || elapsed_us > UINT64_C(86400000000))
        return false;
    *seconds = base + elapsed_us / UINT64_C(1000000);
    return true;
}

#include "runtime_selftest.h"
#include "platform_runtime.h"
#include "FreeRTOS.h"
#include "task.h"
#include "event_groups.h"
#include <cache.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

struct context_trace {
    volatile uint32_t *owner;
    unsigned transitions;
};
extern unsigned platform_context_probe(uint32_t seed, unsigned iterations, struct context_trace *trace);

#define SELFTEST_GO   (1U << 0)
#define SELFTEST_A    (1U << 1)
#define SELFTEST_B    (1U << 2)

static EventGroupHandle_t events;
static volatile int active;
static volatile int stop_requested;
static volatile uint32_t live_probe;
struct probe_state {
    uint32_t seed;
    EventBits_t done;
    unsigned error;
    unsigned rounds;
    volatile unsigned heartbeat;
    volatile unsigned finished;
    struct context_trace trace;
};
static struct probe_state probes[2];

static void probe_task(void *argument)
{
    struct probe_state *state = argument;
    xEventGroupWaitBits(events, SELFTEST_GO, pdFALSE, pdTRUE, portMAX_DELAY);
    for (state->rounds = 0; state->rounds < 32; ++state->rounds) {
        state->error = platform_context_probe(state->seed, 50000, &state->trace);
        if (state->error) break;
    }
    xEventGroupSetBits(events, state->done);
    while (!stop_requested) {
        ++state->heartbeat;
        vTaskDelay(1);
    }
    state->finished = 1;
    vTaskDelete(NULL);
}

static void monitor_task(void *argument)
{
    xEventGroupWaitBits(events, SELFTEST_A | SELFTEST_B, pdFALSE, pdTRUE, portMAX_DELAY);
    unsigned before = probes[0].heartbeat + probes[1].heartbeat;
    int passed = !probes[0].error && !probes[1].error &&
                 probes[0].trace.transitions > 1 && probes[1].trace.transitions > 1;

    /* Larger than the entire 58 MiB application window: must fail, without
     * leaving vTaskSuspendAll nested or preventing subsequent task wakeups. */
    void *oversized = malloc(64U * 1024U * 1024U);
    if (oversized) { free(oversized); passed = 0; }
    if (xTaskGetSchedulerState() != taskSCHEDULER_RUNNING) passed = 0;
    vTaskDelay(25);
    unsigned after = probes[0].heartbeat + probes[1].heartbeat;
    if (after <= before || !probes[0].heartbeat || !probes[1].heartbeat) passed = 0;

    /* Release both workers before destroying their shared event group. */
    stop_requested = 1;
    while (!probes[0].finished || !probes[1].finished) vTaskDelay(1);
    printf("RTOS_SELFTEST %s: context=%u/%u rounds=%u/%u preemptions=%u/%u post_oom_heartbeats=%u isr_unused=%u\n",
           passed ? "PASS" : "FAIL", probes[0].error, probes[1].error,
           probes[0].rounds, probes[1].rounds,
           probes[0].trace.transitions, probes[1].trace.transitions, after - before,
           (unsigned)platform_isr_stack_unused());
    vEventGroupDelete(events);
    events = NULL;
    active = 0;
    vTaskDelete(NULL);
}

int runtime_selftest_start(void)
{
    TaskHandle_t a = NULL, b = NULL, monitor = NULL;
    if (xTaskGetSchedulerState() != taskSCHEDULER_RUNNING) return -EPERM;
    taskENTER_CRITICAL();
    if (active) { taskEXIT_CRITICAL(); return -EBUSY; }
    active = 1;
    taskEXIT_CRITICAL();
    probes[0] = (struct probe_state){ .seed = 0x13579bdfU, .done = SELFTEST_A };
    probes[1] = (struct probe_state){ .seed = 0x2468ace0U, .done = SELFTEST_B };
    probes[0].trace.owner = &live_probe;
    probes[1].trace.owner = &live_probe;
    live_probe = 0;
    stop_requested = 0;
    events = xEventGroupCreate();
    if (!events) goto fail;
    if (xTaskCreate(probe_task, "context-a", 1024, &probes[0], 3, &a) != pdPASS) goto fail;
    if (xTaskCreate(probe_task, "context-b", 1024, &probes[1], 3, &b) != pdPASS) goto fail;
    if (xTaskCreate(monitor_task, "rtos-check", 1536, NULL, 4, &monitor) != pdPASS) goto fail;
    printf("RTOS_SELFTEST starting: two competing tasks, GPR/HI/LO/FPU/FCSR, then OOM recovery\n");
    xEventGroupSetBits(events, SELFTEST_GO);
    return 0;
fail:
    if (a) vTaskDelete(a);
    if (b) vTaskDelete(b);
    if (monitor) vTaskDelete(monitor);
    if (events) vEventGroupDelete(events);
    events = NULL;
    active = 0;
    return -ENOMEM;
}

#define MEMORY_TEST_ALIGNMENT 64U
#define MEMORY_TEST_CHUNK (64U * 1024U)
#define MEMORY_TEST_RESERVE (2U * 1024U * 1024U)
#define MEMORY_TEST_MINIMUM (1U * 1024U * 1024U)

_Static_assert(MEMORY_TEST_ALIGNMENT % cpu_dcache_line_size() == 0,
               "RAM test must own complete data-cache lines");
_Static_assert(MEMORY_TEST_ALIGNMENT % cpu_scache_line_size() == 0,
               "RAM test must own complete secondary-cache lines");

static volatile int memory_test_active;

static uint32_t memory_pattern(uintptr_t address, unsigned pattern)
{
    uint32_t value = (uint32_t)(address & 0x1fffffffU) ^ 0xa5a5a5a5U;
    switch (pattern) {
    case 0: return value;
    case 1: return ~value;
    case 2: return 0;
    case 3: return 0xffffffffU;
    case 4: return 0xaaaaaaaaU;
    default: return 0x55555555U;
    }
}

static int memory_mismatch(const char *phase, unsigned pattern,
                           uintptr_t address, uint32_t expected, uint32_t actual)
{
    printf("RAM_TEST FAIL phase=%s pattern=%u uncached=%08lx physical=%08lx expected=%08lx actual=%08lx\n",
           phase, pattern, (unsigned long)address,
           (unsigned long)(address & 0x1fffffffU),
           (unsigned long)expected, (unsigned long)actual);
    return -EIO;
}

int runtime_memory_test(void)
{
    void *allocation = NULL;
    uintptr_t cached, uncached;
    size_t available, bytes;
    uint64_t started;
    int result = 0;

    if (xTaskGetSchedulerState() != taskSCHEDULER_RUNNING) return -EPERM;
    taskENTER_CRITICAL();
    if (memory_test_active) { taskEXIT_CRITICAL(); return -EBUSY; }
    memory_test_active = 1;
    taskEXIT_CRITICAL();

    available = platform_free_heap();
    /* Include alignment slack and malloc metadata in the retained margin. */
    bytes = available > MEMORY_TEST_RESERVE + 128U ?
            (available - MEMORY_TEST_RESERVE - 128U) & ~(size_t)(MEMORY_TEST_ALIGNMENT - 1U) : 0;
    if (bytes >= MEMORY_TEST_MINIMUM)
        allocation = malloc(bytes + MEMORY_TEST_ALIGNMENT - 1U);
    if (!allocation) {
        printf("RAM_TEST unavailable: need >=1MiB contiguous plus 2MiB reserve, free=%lu\n",
               (unsigned long)available);
        memory_test_active = 0;
        return -ENOMEM;
    }
    cached = ((uintptr_t)allocation + MEMORY_TEST_ALIGNMENT - 1U) &
             ~(uintptr_t)(MEMORY_TEST_ALIGNMENT - 1U);
    uncached = (cached & 0x1fffffffU) | 0xa0000000U;
    started = platform_time_us();
    printf("RAM_TEST starting: application free heap only, not full 64MiB; cached=[%08lx,%08lx) uncached=[%08lx,%08lx) bytes=%lu free_remaining=%lu patterns=6 walking=64\n",
           (unsigned long)cached, (unsigned long)(cached + bytes),
           (unsigned long)uncached, (unsigned long)(uncached + bytes),
           (unsigned long)bytes, (unsigned long)platform_free_heap());

    /* Both endpoints lie inside malloc's payload, on complete 64-byte lines.
     * No allocator header, neighboring live object, stack or vector shares a
     * line with this range. Flush old cached contents before KSEG1 writes;
     * never access this payload through KSEG0 while testing it uncached. */
    for (size_t offset = 0; offset < bytes; offset += MEMORY_TEST_CHUNK) {
        size_t count = bytes - offset;
        if (count > MEMORY_TEST_CHUNK) count = MEMORY_TEST_CHUNK;
        /* The SDK wrapper's __fast_iob reads physical address zero. Use its
         * underlying range operations plus sync to stay within this block. */
        flush_dcache_range(cached + offset, cached + offset + count);
        flush_scache_range(cached + offset, cached + offset + count);
        __asm__ volatile("sync" ::: "memory");
        vTaskDelay(1);
    }
    __asm__ volatile("sync" ::: "memory");

    volatile uint32_t *first = (volatile uint32_t *)uncached;
    for (unsigned inverse = 0; inverse < 2; ++inverse) {
        for (unsigned bit = 0; bit < 32; ++bit) {
            uint32_t expected = 1U << bit;
            if (inverse) expected = ~expected;
            *first = expected;
            __asm__ volatile("sync" ::: "memory");
            uint32_t actual = *first;
            if (actual != expected) {
                result = memory_mismatch("walking", inverse * 32U + bit,
                                         uncached, expected, actual);
                goto done;
            }
        }
    }

    for (unsigned pattern = 0; pattern < 6; ++pattern) {
        for (size_t offset = 0; offset < bytes; offset += MEMORY_TEST_CHUNK) {
            size_t count = bytes - offset;
            if (count > MEMORY_TEST_CHUNK) count = MEMORY_TEST_CHUNK;
            volatile uint32_t *words = (volatile uint32_t *)(uncached + offset);
            for (size_t i = 0; i < count / sizeof(*words); ++i)
                words[i] = memory_pattern(uncached + offset + i * sizeof(*words), pattern);
            __asm__ volatile("sync" ::: "memory");
            vTaskDelay(1);
        }
        for (size_t offset = 0; offset < bytes; offset += MEMORY_TEST_CHUNK) {
            size_t count = bytes - offset;
            if (count > MEMORY_TEST_CHUNK) count = MEMORY_TEST_CHUNK;
            volatile uint32_t *words = (volatile uint32_t *)(uncached + offset);
            for (size_t i = 0; i < count / sizeof(*words); ++i) {
                uintptr_t address = uncached + offset + i * sizeof(*words);
                uint32_t expected = memory_pattern(address, pattern);
                uint32_t actual = words[i];
                if (actual != expected) {
                    result = memory_mismatch("block", pattern, address, expected, actual);
                    goto done;
                }
            }
            vTaskDelay(1);
        }
        printf("RAM_TEST pattern=%u/6 verified bytes=%lu\n", pattern + 1U, (unsigned long)bytes);
    }

done:
    __asm__ volatile("sync" ::: "memory");
    /* Discard any clean cached aliases before returning the allocation to
     * malloc. Use range operations, not the SDK's whole-cache shortcut. */
    for (size_t offset = 0; offset < bytes; offset += MEMORY_TEST_CHUNK) {
        size_t count = bytes - offset;
        if (count > MEMORY_TEST_CHUNK) count = MEMORY_TEST_CHUNK;
        invalid_dcache_range(cached + offset, cached + offset + count);
        invalid_scache_range(cached + offset, cached + offset + count);
        __asm__ volatile("sync" ::: "memory");
        vTaskDelay(1);
    }
    free(allocation);
    if (!result)
        printf("RAM_TEST PASS cached=[%08lx,%08lx) uncached=[%08lx,%08lx) bytes=%lu patterns=6 walking=64 elapsed_us=%llu scope=application-heap\n",
               (unsigned long)cached, (unsigned long)(cached + bytes),
               (unsigned long)uncached, (unsigned long)(uncached + bytes),
               (unsigned long)bytes,
               (unsigned long long)(platform_time_us() - started));
    memory_test_active = 0;
    return result;
}

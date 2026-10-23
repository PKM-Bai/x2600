/* X2600 PM chapter 41, pp.1290-1294; CLKGR0 bit27: chapter20 p.566.
 * This is a continuous hardware service for network randomization. Its stuck
 * word check and READY status do not quantify cryptographic entropy. */
#include "board_entropy.h"
#include "FreeRTOS.h"
#include "semphr.h"
#include "task.h"
#include "platform_runtime.h"
#include <x2600_hal.h>
#include <errno.h>
#include <string.h>

#define RNG_ENABLE UINT32_C(1)
#define RNG_CLEAR (UINT32_C(1) << 12)
#define RNG_GATE (UINT32_C(1) << 27)
#define RNG_CONFIG (UINT32_C(0x03ff0000) | (UINT32_C(1) << 11) | \
                    (UINT32_C(1023) << 1) | RNG_ENABLE)
#define RNG_TIMEOUT_US UINT64_C(10000)

#if defined(AI_BOARD_ENTROPY_TEST)
uint32_t ai_entropy_test_register_read(unsigned offset);
void ai_entropy_test_register_write(unsigned offset, uint32_t value);
#define register_read ai_entropy_test_register_read
#define register_write ai_entropy_test_register_write
#else
static uint32_t register_read(unsigned offset)
{ return *(volatile uint32_t *)(UINT32_C(0xb0072000) + offset); }
static void register_write(unsigned offset, uint32_t value)
{ *(volatile uint32_t *)(UINT32_C(0xb0072000) + offset) = value; }
#endif

static SemaphoreHandle_t mutex;
static ai_board_entropy_report_t status;
static uint32_t previous_word;
static bool have_previous;

static TickType_t lock_ticks(void)
{ return (TickType_t)((50UL * configTICK_RATE_HZ + 999UL) / 1000UL); }

static int take_mutex(void)
{
    if (xTaskGetSchedulerState() != taskSCHEDULER_RUNNING) return -EAGAIN;
    if (mutex == NULL) return -ENODEV;
    return xSemaphoreTake(mutex, lock_ticks()) == pdTRUE ? 0 : -EBUSY;
}

static void release_mutex(void)
{
    BaseType_t result = xSemaphoreGive(mutex);
    configASSERT(result == pdTRUE);
    (void)result;
}

static int hardware_fault(ai_board_entropy_state_t state, int error)
{
    status.state = state;
    status.fault_error = error;
    register_write(0, RNG_CONFIG & ~RNG_ENABLE);
    return error;
}

static int read_word(uint64_t deadline, uint32_t *word)
{
    for (;;) {
        if (platform_time_us() >= deadline)
            return hardware_fault(AI_ENTROPY_FAULT_TIMEOUT, -ETIMEDOUT);
        if (register_read(8) & UINT32_C(1)) break;
    }
    uint32_t value = register_read(4);
    register_write(0, RNG_CONFIG | RNG_CLEAR);
    register_write(0, RNG_CONFIG);
    ++status.hardware_samples;
    if (have_previous && value == previous_word)
        return hardware_fault(AI_ENTROPY_FAULT_REPEAT, -EIO);
    previous_word = value;
    have_previous = true;
    *word = value;
    return 0;
}

int ai_board_entropy_init(void)
{
    /* Startup owns creation: this function is not concurrently initialized. */
    if (xTaskGetSchedulerState() != taskSCHEDULER_RUNNING) return -EAGAIN;
    if (mutex == NULL) {
        mutex = xSemaphoreCreateMutex();
        if (mutex == NULL) return -ENOMEM;
    }
    int result = take_mutex();
    if (result) return result;
    if (status.state != AI_ENTROPY_OFF) {
        result = status.state == AI_ENTROPY_READY ? 0 : status.fault_error;
        goto out;
    }
    unsigned long flags = _local_irq_save();
    uint32_t old_gate = CPM_Instance->CLKGR0 & RNG_GATE;
    CPM_Instance->CLKGR0 &= ~RNG_GATE;
    (void)CPM_Instance->CLKGR0;
    local_irq_restore(flags);
    if (register_read(0) & RNG_ENABLE) {
        flags = _local_irq_save();
        if (old_gate) CPM_Instance->CLKGR0 |= RNG_GATE;
        local_irq_restore(flags);
        result = -EBUSY;
        goto out;
    }
    register_write(0, RNG_CONFIG | RNG_CLEAR);
    register_write(0, RNG_CONFIG);
    status.configuration = register_read(0);
    uint64_t deadline = platform_time_us() + RNG_TIMEOUT_US;
    uint32_t discard;
    result = read_word(deadline, &discard);
    if (!result) result = read_word(deadline, &discard);
    if (!result) status.state = AI_ENTROPY_READY;
out:
    release_mutex();
    return result;
}

bool ai_board_entropy_lwip(void *context, void *data, size_t length)
{
    unsigned char *output = data;
    size_t offset = 0;
    (void)context;
    if (output == NULL || length == 0 || length > AI_BOARD_ENTROPY_MAX_BYTES)
        return false;
    memset(output, 0, length);
    if (take_mutex()) return false;
    bool success = false;
    if (status.state != AI_ENTROPY_READY) goto out;
    ++status.read_calls;
    uint64_t deadline = platform_time_us() + RNG_TIMEOUT_US;
    while (offset < length) {
        uint32_t word;
        if (read_word(deadline, &word)) goto out;
        for (unsigned i = 0; i < 4 && offset < length; ++i) {
            output[offset++] = (unsigned char)word;
            word >>= 8;
        }
    }
    status.bytes_delivered += (uint32_t)length;
    success = true;
out:
    if (!success) memset(output, 0, length);
    release_mutex();
    return success;
}

int ai_board_entropy_get_status(ai_board_entropy_report_t *report)
{
    if (report == NULL) return -EINVAL;
    memset(report, 0, sizeof(*report));
    if (mutex == NULL) return 0;
    int result = take_mutex();
    if (result) return result;
    *report = status;
    release_mutex();
    return 0;
}

int ai_board_entropy_tls(void *context, unsigned char *output, size_t length,
                         size_t *entropy_bits)
{
    (void)context;
    if (entropy_bits) *entropy_bits = 0;
    if (output && length) memset(output, 0, length);
    if (entropy_bits == NULL || (output == NULL && length)) return -EINVAL;
    return -ENOSYS;
}

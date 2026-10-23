#include "board_entropy.h"
#include "FreeRTOS.h"
#include "semphr.h"
#include "task.h"
#include "x2600_hal.h"
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>

ai_entropy_test_cpm_t ai_entropy_test_cpm = { UINT32_MAX };
static uint32_t words[32];
static unsigned word_count, word_at, word_pending, register_reads;
static unsigned mutex_created, locked, fail_create, fail_lock, irq_depth;
static BaseType_t scheduler = taskSCHEDULER_RUNNING;
static uint32_t config = UINT32_C(0x03ff0000);
static uint64_t now_us, word_ready_us, word_interval_us;
static ai_board_entropy_report_t report;

SemaphoreHandle_t xSemaphoreCreateMutex(void)
{
    ++mutex_created;
    return fail_create ? NULL : &locked;
}
BaseType_t xSemaphoreTake(SemaphoreHandle_t mutex, TickType_t timeout)
{
    assert(mutex == &locked && !locked);
    assert(timeout > 0 && timeout <= 50);
    if (fail_lock) return pdFALSE;
    locked = 1;
    return pdTRUE;
}
BaseType_t xSemaphoreGive(SemaphoreHandle_t mutex)
{
    assert(mutex == &locked && locked);
    locked = 0;
    return pdTRUE;
}
BaseType_t xTaskGetSchedulerState(void) { return scheduler; }
unsigned long _local_irq_save(void)
{
    assert(!irq_depth);
    irq_depth = 1;
    return 0;
}
void local_irq_restore(unsigned long flags)
{
    assert(irq_depth == 1 && flags == 0);
    irq_depth = 0;
}
uint64_t platform_time_us(void) { now_us += 10; return now_us; }
uint32_t ai_entropy_test_register_read(unsigned offset)
{
    assert(locked);
    ++register_reads;
    if (offset == 0) return config;
    if (offset == 8) return word_at < word_count && now_us >= word_ready_us;
    assert(offset == 4 && word_at < word_count && !word_pending && now_us >= word_ready_us);
    word_pending = 1;
    return words[word_at];
}
void ai_entropy_test_register_write(unsigned offset, uint32_t value)
{
    assert(locked && offset == 0);
    if (!(config & 1) && (value & 1)) word_ready_us = now_us + word_interval_us;
    config = value;
    if ((value & (1u << 12)) && word_pending) {
        ++word_at;
        word_pending = 0;
        word_ready_us = now_us + word_interval_us;
    }
}
static void seed(unsigned count)
{
    assert(count <= sizeof(words) / sizeof(words[0]));
    word_count = count;
    for (unsigned i = 0; i < count; ++i)
        words[i] = UINT32_C(0x10203040) + i;
}
static void get_status(ai_board_entropy_state_t expected)
{
    assert(!ai_board_entropy_get_status(&report));
    assert(report.state == expected);
    assert(!locked && !irq_depth);
}
static void assert_zero(const unsigned char *data, size_t length)
{ for (size_t i = 0; i < length; ++i) assert(data[i] == 0); }

static void startup(void)
{
    get_status(AI_ENTROPY_OFF);
    seed(5);
    words[2] = UINT32_C(0x11223344);
    words[3] = UINT32_C(0x55667788);
    words[4] = UINT32_C(0xaabbccdd);
    assert(!ai_board_entropy_init());
    assert(!ai_board_entropy_init());
    assert(mutex_created == 1);
    get_status(AI_ENTROPY_READY);
    assert(report.hardware_samples == 2 && !report.bytes_delivered);
    assert(report.configuration == UINT32_C(0x03ff0fff));
    assert(ai_entropy_test_cpm.CLKGR0 == (UINT32_MAX & ~(1u << 27)));
    unsigned char output[6];
    const unsigned char expected[] = {0x44, 0x33, 0x22, 0x11, 0x88, 0x77};
    assert(ai_board_entropy_lwip(NULL, output, sizeof(output)));
    assert(!memcmp(output, expected, sizeof(expected)));
    assert(ai_board_entropy_lwip(NULL, output, 4));
    get_status(AI_ENTROPY_READY);
    assert(report.hardware_samples == 5 && report.bytes_delivered == 10 && report.read_calls == 2);
}
static void repeat_across_calls(void)
{
    seed(4); words[3] = words[2];
    assert(!ai_board_entropy_init());
    unsigned char output[4];
    assert(ai_board_entropy_lwip(NULL, output, sizeof(output)));
    assert(!ai_board_entropy_lwip(NULL, output, sizeof(output)));
    assert_zero(output, sizeof(output));
    get_status(AI_ENTROPY_FAULT_REPEAT);
    assert(report.fault_error == -EIO && report.hardware_samples == 4);
    assert(report.bytes_delivered == 4 && report.read_calls == 2 && !(config & 1));
    unsigned reads = register_reads;
    assert(ai_board_entropy_init() == -EIO);
    assert(!ai_board_entropy_lwip(NULL, output, sizeof(output)));
    assert(register_reads == reads);
}
static void bounded_cadence(void)
{
    seed(10); /* Two startup words, then the maximum eight-word read. */
    word_interval_us = 940;
    assert(!ai_board_entropy_init());
    unsigned char output[AI_BOARD_ENTROPY_MAX_BYTES + 1];
    uint64_t start = now_us;
    assert(ai_board_entropy_lwip(NULL, output, AI_BOARD_ENTROPY_MAX_BYTES));
    assert(now_us - start >= UINT64_C(7520) && now_us - start < UINT64_C(10000));
    get_status(AI_ENTROPY_READY);
    assert(report.hardware_samples == 10 && report.bytes_delivered == 32 && report.read_calls == 1);
    unsigned reads = register_reads;
    memset(output, 0xa5, sizeof(output));
    assert(!ai_board_entropy_lwip(NULL, output, sizeof(output)));
    assert(register_reads == reads);
    for (size_t i = 0; i < sizeof(output); ++i) assert(output[i] == 0xa5);
    get_status(AI_ENTROPY_READY);
    assert(report.hardware_samples == 10 && report.bytes_delivered == 32 && report.read_calls == 1);
}
static void partial_failure(void)
{
    seed(4); words[3] = words[2];
    assert(!ai_board_entropy_init());
    unsigned char output[8];
    memset(output, 0xa5, sizeof(output));
    assert(!ai_board_entropy_lwip(NULL, output, sizeof(output)));
    assert_zero(output, sizeof(output));
    get_status(AI_ENTROPY_FAULT_REPEAT);
    assert(!report.bytes_delivered && report.read_calls == 1);
}
static void timeout(void)
{
    seed(2);
    assert(!ai_board_entropy_init());
    unsigned char output[4];
    uint64_t start = now_us;
    assert(!ai_board_entropy_lwip(NULL, output, sizeof(output)));
    assert_zero(output, sizeof(output));
    assert(now_us - start <= UINT64_C(10020));
    get_status(AI_ENTROPY_FAULT_TIMEOUT);
    assert(report.fault_error == -ETIMEDOUT && report.hardware_samples == 2);
    assert(!(config & 1));
    assert(ai_board_entropy_init() == -ETIMEDOUT);
}
static void startup_timeout(void)
{
    assert(ai_board_entropy_init() == -ETIMEDOUT);
    get_status(AI_ENTROPY_FAULT_TIMEOUT);
    assert(!report.hardware_samples && now_us <= UINT64_C(10020));
}
static void startup_repeat(void)
{
    seed(2); words[1] = words[0];
    assert(ai_board_entropy_init() == -EIO);
    get_status(AI_ENTROPY_FAULT_REPEAT);
    assert(report.hardware_samples == 2 && !(config & 1));
}
static void busy_owner(void)
{
    config |= 1;
    uint32_t before = config;
    assert(ai_board_entropy_init() == -EBUSY);
    get_status(AI_ENTROPY_OFF);
    assert(config == before && ai_entropy_test_cpm.CLKGR0 == UINT32_MAX);
    assert(register_reads == 1);
}
static void lock_timeout(void)
{
    seed(3);
    assert(!ai_board_entropy_init());
    unsigned char output[4];
    memset(output, 0xa5, sizeof(output));
    unsigned reads = register_reads;
    fail_lock = 1;
    assert(!ai_board_entropy_lwip(NULL, output, sizeof(output)));
    assert_zero(output, sizeof(output));
    assert(ai_board_entropy_get_status(&report) == -EBUSY);
    assert(register_reads == reads);
    fail_lock = 0;
    get_status(AI_ENTROPY_READY);
    assert(ai_board_entropy_lwip(NULL, output, sizeof(output)));
}
static void unavailable(void)
{
    unsigned char output[4];
    memset(output, 0xa5, sizeof(output));
    assert(!ai_board_entropy_lwip(NULL, output, sizeof(output)));
    assert_zero(output, sizeof(output));
    scheduler = taskSCHEDULER_NOT_STARTED;
    assert(ai_board_entropy_init() == -EAGAIN && !mutex_created);
    scheduler = taskSCHEDULER_RUNNING;
    fail_create = 1;
    assert(ai_board_entropy_init() == -ENOMEM && !register_reads);
    get_status(AI_ENTROPY_OFF);
    assert(ai_board_entropy_get_status(NULL) == -EINVAL);
    assert(!ai_board_entropy_lwip(NULL, NULL, 4));
    assert(!ai_board_entropy_lwip(NULL, output, 0));
    assert(!ai_board_entropy_lwip(NULL, output, AI_BOARD_ENTROPY_MAX_BYTES + 1));
}
static void tls_unqualified(void)
{
    seed(3);
    assert(!ai_board_entropy_init());
    unsigned char output[32];
    size_t bits = 256;
    unsigned reads = register_reads;
    memset(output, 0xa5, sizeof(output));
    assert(ai_board_entropy_tls(NULL, output, sizeof(output), &bits) == -ENOSYS);
    assert(bits == 0 && register_reads == reads);
    assert_zero(output, sizeof(output));
    get_status(AI_ENTROPY_READY);
    assert(report.hardware_samples == 2 && !report.bytes_delivered);
    assert(ai_board_entropy_tls(NULL, output, sizeof(output), NULL) == -EINVAL);
}

int main(int argc, char **argv)
{
    assert(argc == 2);
    if (!strcmp(argv[1], "startup")) startup();
    else if (!strcmp(argv[1], "cadence")) bounded_cadence();
    else if (!strcmp(argv[1], "repeat")) repeat_across_calls();
    else if (!strcmp(argv[1], "partial")) partial_failure();
    else if (!strcmp(argv[1], "timeout")) timeout();
    else if (!strcmp(argv[1], "startup-timeout")) startup_timeout();
    else if (!strcmp(argv[1], "startup-repeat")) startup_repeat();
    else if (!strcmp(argv[1], "busy")) busy_owner();
    else if (!strcmp(argv[1], "lock-timeout")) lock_timeout();
    else if (!strcmp(argv[1], "unavailable")) unavailable();
    else if (!strcmp(argv[1], "tls")) tls_unqualified();
    else assert(0);
    assert(!locked && !irq_depth);
    printf("PASS %s (mock hardware; no entropy quality claim)\n", argv[1]);
    return 0;
}

/* Register facts: X2600 Programming Manual, chapter 41, pp.1290-1294.
 * Local source: libbare-cpu/projects/x2660-halley/Demos/printer/tpc/tmp/
 * X2600_PM_layout.txt, lines 83172-83348; clock gate bit: p.566.
 * This is a bounded diagnostic, not an entropy-quality certification. */
#include "board_rng.h"
#include "platform_runtime.h"
#include <x2600_hal.h>
#include <errno.h>
#include <string.h>

#define RNG_REGISTER(offset) (*(volatile uint32_t *)(UINT32_C(0xb0072000) + (offset)))
#define RNG_ENABLE UINT32_C(1)
#define RNG_CLEAR (UINT32_C(1) << 12)
#define RNG_GATE (UINT32_C(1) << 27)

int ai_board_rng_probe(unsigned sample_count, ai_rng_probe_t *report)
{
    if (!report || sample_count < 2 || sample_count > 1024)
        return -EINVAL;
    memset(report, 0, sizeof(*report));
    unsigned long flags = _local_irq_save();
    uint32_t old_gate = CPM_Instance->CLKGR0 & RNG_GATE;
    CPM_Instance->CLKGR0 &= ~RNG_GATE;
    (void)CPM_Instance->CLKGR0;
    local_irq_restore(flags);
    uint32_t old_config = RNG_REGISTER(0);
    int result = 0;
    if (old_config & RNG_ENABLE) {
        result = -EBUSY;
        goto restore_gate;
    }
    /* Enable all ten inverter lines, mask interrupts, maximal documented
     * divider for initial sampling. PCLK/(1023+1); no interrupt installation. */
    uint32_t config = UINT32_C(0x03ff0000) | (UINT32_C(1) << 11) |
                      (UINT32_C(1023) << 1) | RNG_ENABLE;
    RNG_REGISTER(0) = config | RNG_CLEAR;
    RNG_REGISTER(0) = config;
    report->configuration = RNG_REGISTER(0);
    uint64_t start = platform_time_us();
    uint32_t first = 0, previous = 0;
    for (unsigned i = 0; i < sample_count; ++i) {
        uint64_t deadline = platform_time_us() + UINT64_C(10000);
        while (!(RNG_REGISTER(8) & 1)) {
            if (platform_time_us() >= deadline) {
                result = -ETIMEDOUT;
                goto restore_config;
            }
        }
        uint32_t word = RNG_REGISTER(4);
        RNG_REGISTER(0) = config | RNG_CLEAR;
        RNG_REGISTER(0) = config;
        if (i == 0)
            first = word;
        else if (word == previous)
            ++report->repeated_neighbors;
        report->varying_bits |= first ^ word;
        report->zero_words += word == 0;
        report->all_one_words += word == UINT32_MAX;
        report->one_bits += (uint32_t)__builtin_popcount(word);
        ++report->samples;
        previous = word;
    }
    if (report->varying_bits == 0)
        result = -EIO;
restore_config:
    report->elapsed_us = (uint32_t)(platform_time_us() - start);
    RNG_REGISTER(0) = old_config;
restore_gate:
    flags = _local_irq_save();
    if (old_gate)
        CPM_Instance->CLKGR0 |= RNG_GATE;
    local_irq_restore(flags);
    return result;
}

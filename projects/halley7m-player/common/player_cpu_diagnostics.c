#include "player_cpu_diagnostics.h"
#include "player_amp.h"

void player_cpu_diagnostics_publish(volatile struct player_cpu_diagnostics_wire *wire,
                                    const player_cpu_raw *raw, uint32_t sample_sequence)
{
    uint32_t sequence = (wire->publish_seq + 1u) | 1u;
    wire->publish_seq = sequence;
    player_amp_barrier();
    wire->sample_sequence = sample_sequence;
    wire->counter_hz = raw->counter_hz;
    wire->counter_low = (uint32_t)raw->counter;
    wire->counter_high = (uint32_t)(raw->counter >> 32);
    wire->idle_low = (uint32_t)raw->idle_counter;
    wire->idle_high = (uint32_t)(raw->idle_counter >> 32);
    wire->heap_free_bytes = raw->heap_free_bytes;
    wire->heap_min_free_bytes = raw->heap_min_free_bytes;
    wire->heap_total_bytes = raw->heap_total_bytes;
    wire->heap_min_kind = raw->heap_min_kind;
    for (unsigned i = 0; i < 5; ++i) wire->reserved[i] = 0;
    player_amp_barrier();
    wire->publish_seq = sequence + 1u;
    player_amp_barrier();
}

int player_cpu_diagnostics_read(const volatile struct player_cpu_diagnostics_wire *wire,
                                 player_cpu_raw *raw, uint32_t *sample_sequence)
{
    for (unsigned attempt = 0; attempt < 3; ++attempt) {
        uint32_t before = wire->publish_seq;
        if (before & 1u) continue;
        player_amp_barrier();
        player_cpu_raw copy;
        uint32_t sample = wire->sample_sequence;
        copy.counter_hz = wire->counter_hz;
        copy.counter = ((uint64_t)wire->counter_high << 32) | wire->counter_low;
        copy.idle_counter = ((uint64_t)wire->idle_high << 32) | wire->idle_low;
        copy.heap_free_bytes = wire->heap_free_bytes;
        copy.heap_min_free_bytes = wire->heap_min_free_bytes;
        copy.heap_total_bytes = wire->heap_total_bytes;
        copy.heap_min_kind = wire->heap_min_kind;
        player_amp_barrier();
        if (before == wire->publish_seq) {
            if (!sample || !copy.counter_hz) return 0;
            *raw = copy;
            *sample_sequence = sample;
            return 1;
        }
    }
    return 0;
}

#if !defined(__riscv)
int player_cpu_diagnostics_window(player_cpu_sampler *sampler,
                                   const player_cpu_raw *raw, player_cpu_window *window)
{
    if (!raw->counter_hz) return -1;
    if (!sampler->initialized || sampler->previous.counter_hz != raw->counter_hz) {
        sampler->previous = *raw;
        sampler->initialized = 1;
        return 0;
    }
    const player_cpu_raw *previous = &sampler->previous;
    if (raw->counter < previous->counter || raw->idle_counter < previous->idle_counter) {
        sampler->previous = *raw;
        return -1;
    }
    uint64_t elapsed = raw->counter - previous->counter;
    uint64_t idle = raw->idle_counter - previous->idle_counter;
    if (idle > elapsed || elapsed > UINT64_MAX / UINT64_C(10000)) {
        sampler->previous = *raw;
        return -1;
    }
    /* Exactly one second in the source counter's units. */
    if (elapsed < raw->counter_hz) return 0;
    window->elapsed_counter = elapsed;
    window->idle_counter = idle;
    window->counter_end = raw->counter;
    window->counter_hz = raw->counter_hz;
    window->window_us = (elapsed / raw->counter_hz) * UINT64_C(1000000) +
        ((elapsed % raw->counter_hz) * UINT64_C(1000000)) / raw->counter_hz;
    window->busy_basis_points = (uint32_t)(((elapsed - idle) * UINT64_C(10000)) / elapsed);
    window->heap_free_bytes = raw->heap_free_bytes;
    window->heap_min_free_bytes = raw->heap_min_free_bytes;
    window->heap_total_bytes = raw->heap_total_bytes;
    window->heap_min_kind = raw->heap_min_kind;
    window->valid = 1;
    sampler->previous = *raw;
    return 1;
}
#endif

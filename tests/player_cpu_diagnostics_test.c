#include "player_cpu_diagnostics.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static void window_test(void)
{
    player_cpu_sampler sampler = {0};
    player_cpu_window window = {0};
    player_cpu_raw raw = {0};
    raw.counter_hz = 1000000;
    raw.counter = 1000; raw.idle_counter = 200;
    raw.heap_free_bytes = 500; raw.heap_min_free_bytes = 400;
    raw.heap_total_bytes = 1000; raw.heap_min_kind = PLAYER_HEAP_MIN_SAMPLED;
    assert(player_cpu_diagnostics_window(&sampler, &raw, &window) == 0 && !window.valid);
    raw.counter += 500000; raw.idle_counter += 250000;
    assert(player_cpu_diagnostics_window(&sampler, &raw, &window) == 0);
    raw.counter += 500000; raw.idle_counter += 500000;
    assert(player_cpu_diagnostics_window(&sampler, &raw, &window) == 1);
    assert(window.valid && window.busy_basis_points == 2500 && window.window_us == 1000000);
    assert(window.elapsed_counter == 1000000 && window.idle_counter == 750000);
    assert(window.counter_end == raw.counter && window.counter_hz == 1000000);
    assert(window.heap_free_bytes == 500 && window.heap_min_free_bytes == 400);
    assert(window.heap_total_bytes == 1000 && window.heap_min_kind == PLAYER_HEAP_MIN_SAMPLED);
    raw.counter += 1000000;
    assert(player_cpu_diagnostics_window(&sampler, &raw, &window) == 1 && window.busy_basis_points == 10000);
    raw.counter += 1000000; raw.idle_counter += 1000000;
    assert(player_cpu_diagnostics_window(&sampler, &raw, &window) == 1 && window.busy_basis_points == 0);
    raw.counter += 2500000; raw.idle_counter += 1250000;
    assert(player_cpu_diagnostics_window(&sampler, &raw, &window) == 1);
    assert(window.busy_basis_points == 5000 && window.window_us == 2500000);
    /* RV cycles cross 32 bits; neither raw publication nor measurement wraps. */
    raw.counter_hz = 576000000;
    raw.counter = UINT32_MAX - 100u; raw.idle_counter = UINT32_MAX - 300u;
    assert(player_cpu_diagnostics_window(&sampler, &raw, &window) == 0);
    raw.counter += 576000000; raw.idle_counter += 288000000;
    assert(player_cpu_diagnostics_window(&sampler, &raw, &window) == 1);
    assert(window.busy_basis_points == 5000 && window.window_us == 1000000);
    raw.counter += 576000000; raw.idle_counter += 576000001;
    assert(player_cpu_diagnostics_window(&sampler, &raw, &window) == -1);
    --raw.counter;
    assert(player_cpu_diagnostics_window(&sampler, &raw, &window) == -1);
    raw.counter_hz = 0;
    assert(player_cpu_diagnostics_window(&sampler, &raw, &window) == -1);
}

static void wire_test(void)
{
    struct player_cpu_diagnostics_wire wire = {0};
    player_cpu_raw raw = {0}, readback = {0};
    uint32_t sample = 99;
    assert(player_cpu_diagnostics_read(&wire, &readback, &sample) == 0 && sample == 99);
    raw.counter = UINT64_C(0x123456789abcdef0);
    raw.idle_counter = UINT64_C(0x0123456789abcdef);
    raw.counter_hz = 576000000;
    raw.heap_free_bytes = 1234; raw.heap_min_free_bytes = 123;
    raw.heap_total_bytes = 131072; raw.heap_min_kind = PLAYER_HEAP_MIN_ALLOCATOR;
    player_cpu_diagnostics_publish(&wire, &raw, 7);
    assert(player_cpu_diagnostics_read(&wire, &readback, &sample) == 1 && sample == 7);
    assert(readback.counter == raw.counter && readback.idle_counter == raw.idle_counter);
    assert(readback.counter_hz == raw.counter_hz && readback.heap_free_bytes == 1234);
    assert(readback.heap_min_free_bytes == 123 && readback.heap_total_bytes == 131072);
    assert(readback.heap_min_kind == PLAYER_HEAP_MIN_ALLOCATOR);
    wire.publish_seq |= 1;
    assert(player_cpu_diagnostics_read(&wire, &readback, &sample) == 0);
    wire.publish_seq = UINT32_MAX - 1u;
    player_cpu_diagnostics_publish(&wire, &raw, 8);
    assert(wire.publish_seq == 0);
    assert(player_cpu_diagnostics_read(&wire, &readback, &sample) == 1 && sample == 8);
    for (unsigned i = 0; i < 5; ++i) assert(wire.reserved[i] == 0);
}

int main(void)
{
    window_test(); wire_test();
    puts("PASS CPU diagnostics: >=1s real counter windows, idle deltas, 0/25/50/100% load, RV 64-bit cycles, bounded seqlock, heap source tags");
    return 0;
}

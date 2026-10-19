#ifndef PLAYER_CPU_DIAGNOSTICS_H
#define PLAYER_CPU_DIAGNOSTICS_H

#include <stdint.h>

#define PLAYER_CPU_WINDOW_US UINT32_C(1000000)
enum player_heap_min_kind {
    PLAYER_HEAP_MIN_ALLOCATOR = 1, PLAYER_HEAP_MIN_SAMPLED = 2
};

typedef struct player_cpu_raw {
    uint64_t counter;
    uint64_t idle_counter;
    uint32_t counter_hz;
    uint32_t heap_free_bytes;
    uint32_t heap_min_free_bytes;
    uint32_t heap_total_bytes;
    uint32_t heap_min_kind;
} player_cpu_raw;

/* Cross-core ABI contains 32-bit words only. Writer owns publish_seq; readers
 * accept an equal, even sequence around a fenced copy. sample_sequence=0 means
 * unavailable. Counter units are counter_hz ticks per second, never heartbeat.
 */
struct player_cpu_diagnostics_wire {
    uint32_t publish_seq;
    uint32_t sample_sequence;
    uint32_t counter_hz;
    uint32_t counter_low, counter_high;
    uint32_t idle_low, idle_high;
    uint32_t heap_free_bytes;
    uint32_t heap_min_free_bytes;
    uint32_t heap_total_bytes;
    uint32_t heap_min_kind;
    uint32_t reserved[5];
};
_Static_assert(sizeof(struct player_cpu_diagnostics_wire) == 64, "CPU diagnostic ABI");

typedef struct player_cpu_window {
    uint64_t window_us;
    uint64_t elapsed_counter;
    uint64_t idle_counter;
    uint64_t counter_end; /* absolute end timestamp in counter_hz units */
    uint32_t counter_hz;
    uint32_t busy_basis_points; /* 10000 = 100.00% */
    uint32_t heap_free_bytes;
    uint32_t heap_min_free_bytes;
    uint32_t heap_total_bytes;
    uint32_t heap_min_kind;
    uint32_t valid;
} player_cpu_window;

typedef struct player_cpu_sampler {
    player_cpu_raw previous;
    uint32_t initialized;
} player_cpu_sampler;

/* Platform collector: call capture from a non-idle task. It reads FreeRTOS's
 * cumulative idle task runtime under a short, nonblocking critical section.
 * ISR time is charged to the interrupted task by FreeRTOS; ISR load while
 * idle is therefore included in idle, not separately measured as busy work.
 */
uint64_t player_cpu_diagnostics_counter(void);
int player_cpu_diagnostics_capture(player_cpu_raw *raw);

void player_cpu_diagnostics_publish(volatile struct player_cpu_diagnostics_wire *wire,
                                    const player_cpu_raw *raw, uint32_t sample_sequence);
/* 1 = stable sample, 0 = unpublished/concurrent writer. Bounded read retries. */
int player_cpu_diagnostics_read(const volatile struct player_cpu_diagnostics_wire *wire,
                                 player_cpu_raw *raw, uint32_t *sample_sequence);
/* MIPS/host only. 1=new >=1s window, 0=needs more time, -1=invalid counters.
 * The RV publishes raw 64-bit counters and does no 64-bit divide/multiply.
 */
int player_cpu_diagnostics_window(player_cpu_sampler *sampler,
                                   const player_cpu_raw *raw, player_cpu_window *window);

#endif

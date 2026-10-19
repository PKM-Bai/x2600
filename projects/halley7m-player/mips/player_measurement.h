#ifndef PLAYER_MEASUREMENT_H
#define PLAYER_MEASUREMENT_H
#include <stdint.h>

#define PLAYER_MEASUREMENT_CAPACITY 1860u
#define PLAYER_MEASUREMENT_MAX_SECONDS 1800u
#define PLAYER_MEASUREMENT_LABEL_BYTES 48u
enum player_measurement_end {
    PLAYER_MEASURE_NONE, PLAYER_MEASURE_MANUAL, PLAYER_MEASURE_DURATION,
    PLAYER_MEASURE_EOF, PLAYER_MEASURE_SESSION_STOP, PLAYER_MEASURE_ERROR,
    PLAYER_MEASURE_FULL
};
enum player_measurement_flags {
    PLAYER_MEASURE_FINAL = 1u, PLAYER_MEASURE_MIPS_FRESH = 2u,
    PLAYER_MEASURE_RISCV_FRESH = 4u, PLAYER_MEASURE_RISCV_RUNNING = 8u,
    PLAYER_MEASURE_SCOPE_CHANGED = 16u, PLAYER_MEASURE_COUNTER_WRAP = 32u,
    PLAYER_MEASURE_RING_INVALID = 64u, PLAYER_MEASURE_AUDIO_INVALID = 128u,
    PLAYER_MEASURE_SNAPSHOT_INVALID = 256u
};
typedef struct player_measurement_info {
    char label[PLAYER_MEASUREMENT_LABEL_BYTES];
    uint32_t active, reason, record_count, record_capacity, record_bytes;
    uint32_t target_seconds, elapsed_ms, mips_windows, riscv_windows;
    uint32_t mips_mean_busy_bp, riscv_mean_busy_bp;
    uint32_t mips_min_heap_free, riscv_min_heap_free;
    uint32_t max_ring_samples, max_pending_samples, max_video_packets, max_audio_packets;
    uint32_t max_queued_pcm_samples, max_late_us;
    uint64_t elapsed_samples, decoded_frames, displayed_frames, dropped_frames, late_frames;
    uint64_t audio_underruns, dma_errors, dma_timeouts;
} player_measurement_info;

/* All calls belong to the same console/media worker that services audio.
 * Main polls diagnostics first; this module only reads its cached latest
 * report. No tasks, playback, volume changes, or UART output are initiated.
 * One-second records are in fixed RAM; a final partial interval is explicitly
 * flagged FINAL. Reaching EOF/STOP/error after observed playback ends the run.
 */
int player_measurement_begin(const char *label, uint32_t duration_seconds);
int player_measurement_step(void); /* 1 newly ended, 0 collecting/inactive */
int player_measurement_stop(void); /* 1 ended, 0 already inactive */
void player_measurement_get_info(player_measurement_info *info);
/* Only explicit caller requests print. dump refuses an active run (-EBUSY);
 * normally stop playback before a long UART dump. No automatic media STOP. */
void player_measurement_print_summary(void);
/* Optional non-UART pump runs once before each data row, e.g. AMP heartbeat.
 * The caller first confirms media/audio are stopped before a lengthy dump. */
int player_measurement_dump_csv(void (*pump)(void));
#endif

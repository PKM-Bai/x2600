#ifndef PLAYER_DISPLAY_SAMPLE_H
#define PLAYER_DISPLAY_SAMPLE_H
#include <stdint.h>

#define PLAYER_DISPLAY_SAMPLE_MAX 16u
#define PLAYER_DISPLAY_SAMPLE_CUSTOM512 0u
#define PLAYER_DISPLAY_SAMPLE_LINEAR 1u

typedef struct { uint32_t x, y; } player_display_sample_point_t;
typedef struct {
    uint32_t model, slot, sequence, physical, count;
    uint32_t argb[PLAYER_DISPLAY_SAMPLE_MAX];
} player_display_sample_t;

/* Read-only diagnostic for a completed, still pinned CUSTOM surface. Call
 * after PAUSED/ENDED and pending GUI work has drained. Native direct output
 * returns -ENOTSUP; producer/route work returns -EAGAIN. No buffer ownership,
 * descriptors or pixel data are changed. At most sixteen uncached word reads
 * share one short critical section. Model0 uses the board-verified CUSTOM
 * layout: column-major 512-byte row blocks (128 ARGB pixels per block).
 * Model1 is a raw linear-address diagnostic bridge, not the CUSTOM layout. */
int player_display_sample_custom(const player_display_sample_point_t *points,
                                 uint32_t count, uint32_t model,
                                 player_display_sample_t *sample);
#endif

#ifndef PLAYER_DISPLAY_H
#define PLAYER_DISPLAY_H

#include "player_video.h"

typedef struct {
    const void *nv12;                 /* Aligned cached/uncached DDR alias. */
    uint32_t capacity;
    player_video_token_t token;
} player_display_frame_t;

typedef struct {
    player_video_token_t latched;
    bool has_retired;
    player_video_token_t retired;
    uint32_t irq_sequence;
    uint64_t irq_time_us;
    uint32_t descriptor, physical_y, read_site;
} player_display_completion_t;

typedef struct {
    uint32_t vblanks, underruns, confirmed, mismatches;
    uint32_t last_descriptor, last_physical_y, last_read_site;
    bool pending, scanout;
} player_display_stats_t;

/* All public calls run on the one media/display owner task. Init/start do not
 * allocate pixel buffers. Start enables submissions; first confirmed frame
 * turns on the existing panel backlight when its completion is polled. */
int player_display_init(void);
int player_display_start(void);
/* The producer must publish pixels to DDR before submission. This backend
 * cleans descriptors only, to avoid overwriting decoder DMA with stale cache.
 * Returns -EAGAIN until the previous submit is confirmed and polled. The
 * caller retains all display holds until an IRQ-confirmed completion/stop. */
int player_display_submit_frame(const player_display_frame_t *frame);
/* Returns 1 for a completion, 0 for none, or a negative errno. Completion
 * confirms latched and retires the prior display hold, never codec holds.
 * No player_video_pool operation is performed inside this driver or its ISR. */
int player_display_poll_completion(player_display_completion_t *completion);
void player_display_get_stats(player_display_stats_t *stats);
void player_display_print_status(void);
int player_display_test_pattern(bool enabled);
/* Success confirms composer/SRDMA idle: all outstanding display holds can be
 * cleared by the owner. On timeout keep their buffers pinned and retry stop. */
int player_display_stop(void);

#endif

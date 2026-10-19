#ifndef PLAYER_DISPLAY_H
#define PLAYER_DISPLAY_H

#include "player_video.h"
#define PLAYER_DISPLAY_HAS_OVERLAY 1
#define PLAYER_DISPLAY_HAS_VIDEO_PRIORITY 1
#define PLAYER_DISPLAY_HAS_GEOMETRY 1
#define PLAYER_DISPLAY_OVERLAY_BYTES (720u * 1280u * 4u)

typedef struct {
    const void *nv12;                 /* Aligned cached/uncached DDR alias. */
    uint32_t capacity;
    player_video_token_t token;
} player_display_frame_t;

typedef struct {
    bool has_latched; /* False for an explicit video-layer removal. */
    player_video_token_t latched;
    bool has_retired;
    player_video_token_t retired;
    uint32_t irq_sequence;
    uint64_t irq_time_us;
    uint32_t descriptor, physical_y, read_site;
} player_display_completion_t;

typedef struct {
    uint32_t vblanks, underruns, confirmed, mismatches;
    uint32_t overlay_confirmed;
    uint32_t video_submitted, video_busy;
    uint32_t overlay_submitted, overlay_queued_requests, overlay_merged, overlay_only;
    uint32_t eod_period_min_us, eod_period_max_us, video_latency_max_us;
    uint64_t last_eod_us;
    uint32_t last_descriptor, last_physical_y, last_read_site;
    bool pending, scanout, overlay_scanout, video_active;
    uint32_t width, height, route_generation, route_phase;
    uint32_t writebacks, rotator_sof, rotator_eof, rotator_repeats, rotator_errors;
    int route_error;
} player_display_stats_t;

/* Init/stop run before/after both GUI and media tasks use the driver. Init/start do not
 * allocate pixel buffers. Start enables submissions; first confirmed frame
 * turns on the existing panel backlight when its completion is polled.
 * GUI calls only submit_overlay/poll_overlay_completion; the media worker
 * calls only submit_frame/clear_video/poll_completion. Short critical sections
 * serialize descriptor submission. GUI-only transactions never create a video
 * completion or retire a video hold. Both layers have unique mixer orders. */
int player_display_init(void);
int player_display_start(void);
/* Media owner, after the previous video hold was retired, before first decode.
 * 720x1280 keeps the native direct route. 1280x720 uses CUSTOM -> ROT -> DSI.
 * Retry -EAGAIN from LOADING while the GUI owner drains/rebuilds its surfaces;
 * 0 acknowledges both route and GUI layout. Failure keeps DMA buffers pinned.
 * No LVGL call is made from this function. */
int player_display_set_video_geometry(uint32_t width, uint32_t height);
/* GUI adapter handshake: phase 0 running, 1 drain/pause, 2 old DMA is idle and
 * both surfaces may be rebuilt. Acknowledge the returned generation/phase.
 * Only phase2 permits changing the byte layout or overwriting old front. */
void player_display_gui_attach(bool attached);
int player_display_gui_mode(uint32_t *generation, bool *logical);
int player_display_gui_ack(uint32_t generation, unsigned phase);
uint32_t player_display_overlay_stride(void);
/* Media worker sets true only during continuous PLAYING AVI output. GUI
 * requests then wait to merge into a video submission instead of consuming
 * their own 30Hz refresh. Set false for LOADING/PAUSED/ENDED/STOP/error so GUI
 * polling can publish independently. Default false; no buffer is released. */
void player_display_set_video_active(bool active);
/* The producer must publish pixels to DDR before submission. This backend
 * cleans descriptors only, to avoid overwriting decoder DMA with stale cache.
 * Returns -EAGAIN until the previous submit is confirmed and polled. The
 * caller retains all display holds until an IRQ-confirmed completion/stop. */
int player_display_submit_frame(const player_display_frame_t *frame);
/* Disable video while keeping the current GUI. A successful queued removal
 * emits has_latched=false/has_retired=true only after the new descriptor is
 * confirmed. Poll and retire that hold before freeing the video pool. */
int player_display_clear_video(void);
/* Straight ARGB8888, 64-byte aligned. Native route: 720x1280/stride720;
 * rotated route: logical 1280x720/stride1280. Geometry handshake must complete.
 * Producer publishes dirty pixel data to DDR. Keep BOTH old and submitted
 * surfaces pinned until matching serial is returned; old surface is then
 * writable, current surface stays pinned. Serial must be nonzero. One queued
 * GUI update may be accepted behind a hardware video transaction and merged
 * into the next video descriptor; GUI polling drives it while video_active
 * is false. A continuous video stream owns each hardware refresh slot. */
int player_display_submit_overlay(const uint32_t *argb, uint32_t capacity, uint32_t serial);
int player_display_poll_overlay_completion(uint32_t *serial);
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

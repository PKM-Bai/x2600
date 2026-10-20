#ifndef PLAYER_VIDEO_H
#define PLAYER_VIDEO_H

#include <stdbool.h>
#include <stdint.h>

#define PLAYER_VIDEO_MAX_REFS  6u
/* Six retained references, the current decode and two display holds. */
#define PLAYER_VIDEO_SLOTS     (PLAYER_VIDEO_MAX_REFS + 3u)
#define PLAYER_VIDEO_ALIGNMENT 128u

typedef struct {
    uint32_t width, height, stride;
    uint32_t y_bytes, uv_offset, uv_bytes, total_bytes;
} player_video_layout_t;

/* NV12 uses even dimensions, a 128-pixel stride and 128-byte plane alignment.
 * Return 0, -EINVAL for dimensions, or -EOVERFLOW for a 32-bit size overflow. */
int player_video_layout_nv12(uint32_t width, uint32_t height,
                              player_video_layout_t *layout);
bool player_video_buffer_valid(const player_video_layout_t *layout,
                                uintptr_t base, uint32_t capacity);

enum {
    PLAYER_VIDEO_CODEC   = 1u,
    PLAYER_VIDEO_READY   = 2u,
    PLAYER_VIDEO_PENDING = 4u,
    PLAYER_VIDEO_SCANOUT = 8u
};

typedef struct {
    uint32_t session;
    uint32_t generation;
    uint32_t slot;
} player_video_token_t;

typedef struct {
    uint32_t generation;
    uint32_t holds;
} player_video_slot_t;

/* Metadata only: decoder frames, buffers and DPU descriptors stay with their
 * drivers. All calls must be serialized by the player's existing owner/lock. */
typedef struct {
    player_video_slot_t slots[PLAYER_VIDEO_SLOTS];
    uint32_t session;
} player_video_pool_t;

void player_video_pool_init(player_video_pool_t *pool);
bool player_video_codec_acquire(player_video_pool_t *pool,
                                 player_video_token_t *token);
/* Publishing retains the codec hold, e.g. while the decoder uses a DPB ref. */
bool player_video_codec_publish(player_video_pool_t *pool,
                                 player_video_token_t token);
bool player_video_codec_release(player_video_pool_t *pool,
                                 player_video_token_t token);
bool player_video_discard_ready(player_video_pool_t *pool,
                                 player_video_token_t token);
/* At most one pending and one scanout frame. Latch retires the former scanout
 * hold, and must run only after hardware confirms the new frame is scanning. */
bool player_video_display_submit(player_video_pool_t *pool,
                                  player_video_token_t token);
bool player_video_display_latch(player_video_pool_t *pool,
                                 player_video_token_t token);
/* Release/cancel a display hold only after hardware no longer uses the slot. */
bool player_video_display_release(player_video_pool_t *pool,
                                   player_video_token_t token);

/* Call after decoder and DPU have stopped and released hardware references.
 * Invalidate the session and clear holds; old callbacks cannot touch new slots.
 * Do not re-init an existing pool between sessions, since that resets tokens. */
void player_video_pool_stop(player_video_pool_t *pool);

#endif

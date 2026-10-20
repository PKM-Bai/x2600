#ifndef PLAYER_VIDEO_H
#define PLAYER_VIDEO_H

#include <stdbool.h>
#include <stdint.h>

#define PLAYER_VIDEO_MAX_REFS  6u
/* Six retained references, the current decode and two display holds. */
#define PLAYER_VIDEO_SLOTS     (PLAYER_VIDEO_MAX_REFS + 3u)
#define PLAYER_VIDEO_ALIGNMENT 128u

/* Coded NV12 memory and its visible SPS rectangle are separate. Matrix uses
 * ISO/H264 values: 1=709, 5/6=601, 2=unspecified. full_range=-1 is unspecified,
 * 0 limited, 1 full. Missing SAR is 0/0, interpreted as square pixels. */
typedef struct {
    uint32_t coded_width, coded_height, visible_width, visible_height;
    uint32_t crop_left, crop_top, sar_num, sar_den, matrix_coefficients;
    int32_t full_range;
    uint32_t colour_primaries, transfer_characteristics; /* ISO; 2 unspecified. */
} player_video_format_t;

/* Merge each property independently, keeping known SPS values. Container
 * geometry is ignored. Remaining unknown matrix/range/SAR is left for display
 * inference. Explicit unsupported matrices, primaries or transfer curves are
 * rejected; no BT2020 conversion or HDR tone mapping is implemented. */
int player_video_format_resolve(const player_video_format_t *sps,
                                 const player_video_format_t *container,
                                 player_video_format_t *resolved);

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

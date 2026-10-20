#ifndef PLAYER_CODEC_H
#define PLAYER_CODEC_H

#include <stdint.h>
#include "player_video.h"

typedef struct player_codec player_codec_t;

typedef struct {
    uint8_t *base;                 /* Cached, 128-byte aligned contiguous NV12. */
    uint32_t capacity;
} player_codec_buffer_t;

typedef struct {
    player_video_token_t token;
    uint8_t *y, *uv;
    uint32_t physical_y, physical_uv;
    uint32_t width, height, stride;
    uint64_t frame_index;
    uint64_t output_index;         /* Presentation-order ordinal, distinct from AU index. */
    int64_t pts_us, duration_us;
    uint32_t key_frame;
} player_codec_frame_t;

/* All calls and player_video_pool operations share the application's owner.
 * The pool and PLAYER_VIDEO_SLOTS buffers outlive codec and DPU scanout holds. */
int player_codec_open(player_codec_t **codec, player_video_pool_t *pool,
                      uint32_t session, const player_codec_buffer_t buffers[PLAYER_VIDEO_SLOTS]);
int player_codec_open_geometry(player_codec_t **codec, player_video_pool_t *pool,
                               uint32_t session, const player_codec_buffer_t buffers[PLAYER_VIDEO_SLOTS],
                               uint32_t width, uint32_t height);
/* Submit one whole Annex B AU; capacity includes 64 writable padding bytes.
 * Returns -EAGAIN if all slots are held; the caller retains the AU then. */
int player_codec_submit(player_codec_t *codec, uint32_t session,
                        uint8_t *access_unit, uint32_t length, uint32_t capacity,
                        uint64_t frame_index, int64_t pts_us, int64_t duration_us);
/* Flush decoder delay at EOF; get_ready then returns each final frame. */
int player_codec_drain(player_codec_t *codec, uint32_t session);
int player_codec_get_ready(player_codec_t *codec, uint32_t session,
                           player_codec_frame_t *frame);
/* Drop a READY frame. Submitted/scanout frames use the display pool API. */
int player_codec_return_frame(player_codec_t *codec, player_video_token_t token);
/* Stop/reset FELIX before releasing DPB references. Preserves display holds.
 * Caller stops DPU and calls player_video_pool_stop before a new session. */
int player_codec_stop(player_codec_t *codec, uint32_t session);
void player_codec_close(player_codec_t *codec);
/* Successful input AUs using the selected +128 -> +127 hardware policy.
 * Multiple affected slices count once; NULL returns zero. */
uint32_t player_codec_weight_approximation_count(const player_codec_t *codec);
/* SDK HAL exposes NV12 as 8; the private decoder exposes it as 2. */
int player_codec_format_from_hal(uint32_t hal_format);

#endif

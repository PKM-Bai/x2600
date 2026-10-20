#ifndef PLAYER_CODEC_WORKSPACE_H
#define PLAYER_CODEC_WORKSPACE_H

#include "player_video.h"
#include <stddef.h>
#include <stdint.h>

#define PLAYER_CODEC_WORKSPACE_ALIGNMENT 256u

/* FELIX needs tiled reference planes and macroblock side data in addition to
 * the externally owned linear NV12 output. Reserve all of these once while
 * loading; frame-to-frame allocation must not fragment the small DDR heap. */
typedef struct {
    uint32_t width, height, slots;
    uint32_t ctrl_bytes, mv_bytes, y_bytes, uv_bytes;
    uint32_t ctrl_offset, mv_offset, y_offset, uv_offset;
    uint32_t slot_bytes, total_bytes;
} player_codec_workspace_layout_t;

typedef struct {
    uint8_t *ctrl, *mv, *y, *uv;
} player_codec_workspace_regions_t;

typedef struct {
    player_codec_workspace_layout_t layout;
    uint8_t *arena;
    uint32_t active_mask, active_count, peak_active;
} player_codec_workspace_t;

/* Dimensions are the macroblock-aligned coded dimensions, not the visible
 * crop. The player keeps the 720p pixel budget and its fixed output-slot cap. */
int player_codec_workspace_layout(uint32_t width, uint32_t height, uint32_t slots,
                                  player_codec_workspace_layout_t *layout);

/* The codec owns the aligned arena and must keep it until every codec hold
 * has retired. This helper neither allocates nor frees memory. */
int player_codec_workspace_init(player_codec_workspace_t *workspace,
                                const player_codec_workspace_layout_t *layout,
                                void *arena, size_t capacity);
int player_codec_workspace_acquire(player_codec_workspace_t *workspace, uint32_t slot,
                                   player_codec_workspace_regions_t *regions);
int player_codec_workspace_release(player_codec_workspace_t *workspace, uint32_t slot);

#endif

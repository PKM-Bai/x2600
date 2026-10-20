#ifndef PLAYER_DISPLAY_FORMAT_H
#define PLAYER_DISPLAY_FORMAT_H
#include "player_video.h"

typedef struct {
    player_video_format_t format;
    player_video_layout_t memory;
    uint32_t canvas_width, canvas_height;
    uint32_t x, y, width, height, scale_enabled, coef_x, coef_y;
    uint32_t y_offset, uv_offset;
    uint32_t matrix, full_range, matrix_inferred, range_inferred;
    uint32_t mult_yrv, mult_gugv, mult_bu, sub_yuv;
} player_display_format_plan_t;

/* Pure metadata: no IO, heap, hardware, or task calls. */
int player_display_plan_format(const player_video_format_t *format,
                               player_display_format_plan_t *out);
bool player_display_same_format(const player_video_format_t *a,
                                 const player_video_format_t *b);
#endif

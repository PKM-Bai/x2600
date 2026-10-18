/* X2600 FreeRTOS ReGBA display backend. GPL-2.0-or-later. */
#ifndef REGBA_PLATFORM_VIDEO_H
#define REGBA_PLATFORM_VIDEO_H

#include <stdint.h>

/* LCD is off until video_init/video_test_pattern is requested explicitly.
 * Init, test, present and off must run in the same core task. Status only
 * reads counters and may be called by the console task. The native display
 * is 720x1280 portrait XRGB8888. Source stride is in uint16_t pixels and the
 * source uses GBA XBGR1555. Present skips frames until a backbuffer is free;
 * it never waits for the 30 Hz panel from the 60 Hz emulation loop.
 */
int video_init(void);
int video_test_pattern(void);
int video_dsi_pattern(void);
void video_present_gba(const uint16_t *rgb555, unsigned source_stride_pixels);
typedef void (*video_draw_fn)(uint32_t *pixels, unsigned width,
                              unsigned height, void *context);
/* Core task only. Draws a whole native XRGB8888 backbuffer. Returns 0 when
 * queued, -EAGAIN when still owned by LCD DMA, or another negative errno.
 * The caller may retry an unchanged view; this function never waits. */
int video_present_ui(video_draw_fn draw, void *context);
void video_off(void);
void video_status(void);
void video_diagnostics(void);

#endif

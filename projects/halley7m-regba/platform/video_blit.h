/* X2600 FreeRTOS ReGBA portrait renderer. GPL-2.0-or-later. */
#ifndef REGBA_VIDEO_BLIT_H
#define REGBA_VIDEO_BLIT_H

#include <stdint.h>

/* Blit the 240x160 GBA image at 3x nearest-neighbour scale into a native
 * 720x1280 portrait framebuffer: x=0..719, y=400..879, without rotation.
 *
 * rgb555 uses ReGBA's GBA-native XBGR1555 words: R bits 0..4, G bits 5..9,
 * B bits 10..14; bit 15 is ignored. Output words are 0xFFRRGGBB, suitable
 * for ARGB8888/XRGB8888. Both strides are measured in pixels, not bytes:
 * stride_pixels >= 720 and source_stride_pixels >= 240.
 *
 * The caller supplies a writable backbuffer with at least 1280 rows after
 * display initialization, and calls this from the same core task that owns
 * rgb555. It clears the borders once and handles buffer presentation/cache
 * synchronization. Only the 720x480 game region is written here. Buffers
 * must not overlap; no memory is allocated and no hardware is accessed.
 */
void video_blit_gba(uint32_t *framebuffer, unsigned stride_pixels,
                    const uint16_t *rgb555, unsigned source_stride_pixels);

#endif

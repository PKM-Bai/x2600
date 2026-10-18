/* X2600 FreeRTOS ReGBA portrait renderer. GPL-2.0-or-later. */
#include "video_blit.h"

void video_blit_gba(uint32_t *framebuffer, unsigned stride_pixels,
                    const uint16_t *rgb555, unsigned source_stride_pixels)
{
    uint32_t *destination = framebuffer + 400u * stride_pixels;

    for (unsigned y = 0; y < 160; ++y) {
        const uint16_t *source = rgb555 + y * source_stride_pixels;
        uint32_t *row0 = destination;
        uint32_t *row1 = row0 + stride_pixels;
        uint32_t *row2 = row1 + stride_pixels;

        for (unsigned x = 0; x < 240; ++x) {
            uint32_t pixel = source[x];
            uint32_t red = pixel & 31u;
            uint32_t green = (pixel >> 5) & 31u;
            uint32_t blue = (pixel >> 10) & 31u;
            red = (red << 3) | (red >> 2);
            green = (green << 3) | (green >> 2);
            blue = (blue << 3) | (blue >> 2);
            uint32_t color = 0xFF000000u | (red << 16) | (green << 8) | blue;
            unsigned output_x = x * 3u;

            row0[output_x] = row0[output_x + 1] = row0[output_x + 2] = color;
            row1[output_x] = row1[output_x + 1] = row1[output_x + 2] = color;
            row2[output_x] = row2[output_x + 1] = row2[output_x + 2] = color;
        }
        destination += 3u * stride_pixels;
    }
}

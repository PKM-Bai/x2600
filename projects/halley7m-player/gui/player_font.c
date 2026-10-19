/* GNU Unifont adapter. Font data and this derivative: GPL-2.0-or-later. */
#include "lvgl.h"
#include "player_font_data.h"
#include <string.h>

static uint8_t bitmap[128]; /* One 32x32 1bpp glyph; GUI task owns LVGL. */

static const player_font_glyph_t *find_glyph(uint32_t code)
{
    unsigned low = 0, high = PLAYER_FONT_COUNT;
    while (low < high) {
        unsigned middle = low + (high - low) / 2;
        if (player_font_glyphs[middle].code < code) low = middle + 1;
        else high = middle;
    }
    return low < PLAYER_FONT_COUNT && player_font_glyphs[low].code == code
        ? &player_font_glyphs[low] : NULL;
}

static bool describe(const lv_font_t *font, lv_font_glyph_dsc_t *out,
                     uint32_t code, uint32_t next)
{
    (void)font; (void)next;
    if (code < 32) return false;
    const player_font_glyph_t *glyph = find_glyph(code);
    unsigned width = glyph ? glyph->width * 2u : 32u;
    out->adv_w = (uint16_t)(width + 2);
    out->box_w = (uint16_t)width;
    out->box_h = 32;
    out->ofs_x = 0;
    out->ofs_y = 0;
    out->bpp = 1;
    out->is_placeholder = 0; /* Missing BMP/astral glyphs use our visible box. */
    return true;
}

static const uint8_t *get_bitmap(const lv_font_t *font, uint32_t code)
{
    (void)font;
    const player_font_glyph_t *glyph = find_glyph(code);
    unsigned source_width = glyph ? glyph->width : 16u;
    unsigned stride = source_width / 4u;
    memset(bitmap, 0, sizeof(bitmap));
    for (unsigned y = 0; y < 16; ++y) {
        uint16_t row = glyph ? glyph->rows[y] : (y == 0 || y == 15 ? 0xffffu : 0x8001u);
        for (unsigned x = 0; x < source_width; ++x) {
            if (!(row & (0x8000u >> x))) continue;
            for (unsigned dy = 0; dy < 2; ++dy)
                for (unsigned dx = 0; dx < 2; ++dx) {
                    unsigned px = x * 2u + dx;
                    bitmap[(y * 2u + dy) * stride + px / 8u] |= (uint8_t)(0x80u >> (px % 8u));
                }
        }
    }
    return bitmap;
}

const lv_font_t player_font_32 = {
    .get_glyph_dsc = describe, .get_glyph_bitmap = get_bitmap,
    .line_height = 36, .base_line = 4,
    .underline_position = -3, .underline_thickness = 2
};

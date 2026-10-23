/* GNU Unifont adapter and generated data: GPL-2.0-or-later. */
#include "ai_gui.h"
#include "lvgl.h"
#include "ai_boot_font.h"
#include <string.h>
static ai_asset active_font;
static uint8_t bitmap[128];
void ai_gui_font_set(const ai_asset *font)
{
    if (font && font->payload) active_font = *font;
    else {
        (void)ai_asset_open(&active_font, ai_boot_font, sizeof(ai_boot_font), "AIF1");
    }
}
static bool describe(const lv_font_t *font, lv_font_glyph_dsc_t *out, uint32_t code, uint32_t next)
{
    (void)font; (void)next;
    if (code < 32 || (code >= 0xf000 && code <= 0xf8ff)) return false;
    const uint8_t *glyph = ai_font_find(&active_font, code);
    unsigned width = glyph ? glyph[2] * 2u : 32u;
    out->adv_w = (uint16_t)(width + 2); out->box_w = (uint16_t)width;
    out->box_h = 32; out->ofs_x = 0; out->ofs_y = 0; out->bpp = 1; out->is_placeholder = 0;
    return true;
}
static const uint8_t *pixels(const lv_font_t *font, uint32_t code)
{
    (void)font;
    const uint8_t *glyph = ai_font_find(&active_font, code);
    unsigned width = glyph ? glyph[2] : 16, stride = width / 4;
    memset(bitmap, 0, sizeof(bitmap));
    for (unsigned y = 0; y < 16; ++y) {
        uint16_t row = glyph ? (uint16_t)(glyph[4 + y * 2] | (uint16_t)glyph[5 + y * 2] << 8) :
                              (y == 0 || y == 15 ? 0xffffu : 0x8001u);
        for (unsigned x = 0; x < width; ++x) if (row & (0x8000u >> x))
            for (unsigned dy = 0; dy < 2; ++dy) for (unsigned dx = 0; dx < 2; ++dx) {
                unsigned px = x * 2 + dx;
                bitmap[(y * 2 + dy) * stride + px / 8] |= (uint8_t)(0x80u >> (px % 8));
            }
    }
    return bitmap;
}
const lv_font_t ai_font_32 = {.get_glyph_dsc = describe, .get_glyph_bitmap = pixels,
    .line_height = 36, .base_line = 4, .underline_position = -3, .underline_thickness = 2,
    .fallback = &lv_font_montserrat_14};

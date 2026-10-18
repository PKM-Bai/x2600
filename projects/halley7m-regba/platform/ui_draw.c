/* X2600 ReGBA handheld menu renderer. GPL-2.0-or-later. */
#include "ui_draw.h"
#include "ui_font.h"
#include <stddef.h>
#include <stdio.h>

enum {
    BACKGROUND = 0x161122, PANEL = 0x251d39, ROW = 0x2d2342,
    MINT = 0x83f5c3, INK = 0x172c28, TEXT = 0xf6f1ff,
    MUTED = 0xbab0ce, DIM = 0x8e819f, LINE = 0x403151,
    RED = 0xff9aaa
};

typedef struct {
    uint32_t *pixels;
    unsigned width, height;
} canvas_t;

static void rect(canvas_t *c, int x, int y, int width, int height, uint32_t color)
{
    if (x < 0) { width += x; x = 0; }
    if (y < 0) { height += y; y = 0; }
    if (x >= (int)c->width || y >= (int)c->height || width <= 0 || height <= 0)
        return;
    if (width > (int)c->width - x) width = (int)c->width - x;
    if (height > (int)c->height - y) height = (int)c->height - y;
    for (int row = 0; row < height; ++row) {
        uint32_t *out = c->pixels + (size_t)(y + row) * c->width + (unsigned)x;
        for (int col = 0; col < width; ++col) out[col] = 0xff000000u | color;
    }
}

static const ui_font_glyph_t *find_glyph(uint32_t codepoint)
{
    size_t low = 0, high = UI_FONT_GLYPH_COUNT;
    while (low < high) {
        size_t mid = low + (high - low) / 2;
        if (ui_font_glyphs[mid].codepoint < codepoint) low = mid + 1;
        else high = mid;
    }
    if (low < UI_FONT_GLYPH_COUNT && ui_font_glyphs[low].codepoint == codepoint)
        return &ui_font_glyphs[low];
    return NULL;
}

/* Consume one complete UTF-8 character, or one invalid byte. */
static uint32_t next_character(const char **text)
{
    const unsigned char *p = (const unsigned char *)*text;
    unsigned count;
    uint32_t codepoint, minimum;
    if (*p < 0x80) {
        if (*p) ++*text;
        return *p;
    }
    *text += 1;
    if (*p >= 0xc2 && *p <= 0xdf) {
        count = 2; codepoint = *p & 0x1f; minimum = 0x80;
    } else if (*p >= 0xe0 && *p <= 0xef) {
        count = 3; codepoint = *p & 0x0f; minimum = 0x800;
    } else if (*p >= 0xf0 && *p <= 0xf4) {
        count = 4; codepoint = *p & 7; minimum = 0x10000;
    } else return 0xfffd;
    for (unsigned i = 1; i < count; ++i) {
        if ((p[i] & 0xc0) != 0x80) return 0xfffd;
        codepoint = (codepoint << 6) | (p[i] & 0x3f);
    }
    if (codepoint < minimum || codepoint > 0x10ffff ||
            (codepoint >= 0xd800 && codepoint <= 0xdfff)) return 0xfffd;
    *text += count - 1;
    return codepoint;
}

static unsigned character_width(uint32_t codepoint, unsigned scale)
{
    const ui_font_glyph_t *glyph = find_glyph(codepoint);
    return ((glyph ? glyph->width : 16u) + 1u) * scale;
}

static void character(canvas_t *c, int x, int y, uint32_t codepoint,
                      unsigned scale, uint32_t color)
{
    const ui_font_glyph_t *glyph = find_glyph(codepoint);
    if (!glyph) {
        /* A missing filename glyph stays visible without substituting an
         * unrelated character; labels are checked by the font generator. */
        rect(c, x + (int)scale, y + 2 * (int)scale, 12 * (int)scale, (int)scale, color);
        rect(c, x + (int)scale, y + 13 * (int)scale, 12 * (int)scale, (int)scale, color);
        rect(c, x + (int)scale, y + 2 * (int)scale, (int)scale, 12 * (int)scale, color);
        rect(c, x + 12 * (int)scale, y + 2 * (int)scale, (int)scale, 12 * (int)scale, color);
        return;
    }
    for (unsigned row = 0; row < 16; ++row) {
        uint16_t bits = glyph->rows[row];
        for (unsigned col = 0; col < glyph->width; ++col)
            if (bits & (0x8000u >> col))
                rect(c, x + (int)(col * scale), y + (int)(row * scale),
                     (int)scale, (int)scale, color);
    }
}

/* Single-line text never splits UTF-8. Reserve room for an ASCII ellipsis
 * only when truncation is needed, including in long resource paths. */
static void text_line(canvas_t *c, int x, int y, int width, const char *text,
                      unsigned scale, uint32_t color)
{
    if (width <= 0) return;
    const char *scan = text;
    unsigned length = 0;
    while (*scan && *scan != '\n' && *scan != '\r') {
        uint32_t codepoint = next_character(&scan);
        length += character_width(codepoint, scale);
        if (length > (unsigned)width) break;
    }
    int truncated = length > (unsigned)width;
    unsigned dots = character_width('.', scale) * 3u;
    unsigned used = 0;
    while (*text && *text != '\n' && *text != '\r') {
        uint32_t codepoint = next_character(&text);
        unsigned advance = character_width(codepoint, scale);
        if (advance + used + (truncated ? dots : 0u) > (unsigned)width) break;
        character(c, x + (int)used, y, codepoint, scale, color);
        used += advance;
    }
    if (truncated && used + dots <= (unsigned)width) {
        for (unsigned i = 0; i < 3; ++i) {
            character(c, x + (int)used, y, '.', scale, color);
            used += character_width('.', scale);
        }
    }
}

static void text_wrap(canvas_t *c, int x, int y, int width, int height,
                      const char *text, unsigned scale, uint32_t color)
{
    if (width <= 0 || height < (int)(16u * scale)) return;
    int cursor = x, line_y = y;
    const int bottom = y + height, line_height = (int)(20u * scale);
    while (*text) {
        uint32_t codepoint = next_character(&text);
        if (codepoint == '\r') continue;
        if (codepoint == '\n') {
            cursor = x; line_y += line_height;
            continue;
        }
        if (codepoint == '\t') codepoint = ' ';
        unsigned advance = character_width(codepoint, scale);
        if (cursor > x && cursor + (int)advance > x + width) {
            cursor = x; line_y += line_height;
        }
        if (line_y + (int)(16u * scale) > bottom) return;
        character(c, cursor, line_y, codepoint, scale, color);
        cursor += (int)advance;
    }
}

static const char *default_title(const ui_view_t *view)
{
    switch (view->screen) {
    case UI_LIBRARY: return "游戏列表";
    case UI_PAUSE: return "游戏暂停";
    case UI_RESOURCES: return "资源路径";
    case UI_NOTICE: return view->error ? "操作未完成" : "提示";
    case UI_SETTINGS: return "游戏设置";
    }
    return "GBA";
}

void ui_draw_game_stats(uint32_t *pixels, unsigned width, unsigned height,
                        const game_stats_t *stats)
{
    if (!pixels || !stats || !width || height < UI_GAME_STATS_Y + UI_GAME_STATS_HEIGHT)
        return;
    canvas_t canvas = { pixels, width, height };
    char label[64];
    if (stats->valid)
        snprintf(label, sizeof(label), "CPU %lu.%lu%%   FPS %lu.%lu",
                 (unsigned long)(stats->cpu_tenths / 10u),
                 (unsigned long)(stats->cpu_tenths % 10u),
                 (unsigned long)(stats->fps_tenths / 10u),
                 (unsigned long)(stats->fps_tenths % 10u));
    else
        snprintf(label, sizeof(label), "CPU --.-%%   FPS --.-");
    rect(&canvas, 0, UI_GAME_STATS_Y, (int)width, UI_GAME_STATS_HEIGHT, 0);
    text_line(&canvas, 24, UI_GAME_STATS_Y + 8, (int)width - 48, label, 3, TEXT);
}

static const char *default_footer(enum ui_screen screen)
{
    switch (screen) {
    case UI_LIBRARY: return "A 启动    B 返回    L/R 翻页";
    case UI_PAUSE: return "A 确认    B 继续游戏";
    case UI_RESOURCES: return "A 打开目录    B 返回";
    case UI_NOTICE: return "A 确认    B 返回";
    case UI_SETTINGS: return "左/右 调整    A 下一档    B/Y 返回";
    }
    return "A 确认    B 返回";
}

void ui_draw(uint32_t *pixels, unsigned width, unsigned height, void *context)
{
    if (!pixels || !context || !width || !height) return;
    const ui_view_t *view = context;
    canvas_t canvas = { pixels, width, height };
    canvas_t *c = &canvas;
    const int content_width = (int)width - 80;
    const uint32_t accent = view->error ? RED : MINT;
    rect(c, 0, 0, (int)width, (int)height, BACKGROUND);
    rect(c, 32, 28, 64, 6, accent);
    text_line(c, 40, 52, content_width, "GBA", 3, TEXT);
    rect(c, (int)width - 194, 58, 154, 40, PANEL);
    rect(c, (int)width - 182, 73, 8, 8, view->connected ? MINT : DIM);
    text_line(c, (int)width - 162, 62, 112,
              view->connected ? "BT ON" : "BT OFF", 2, view->connected ? MINT : MUTED);
    text_line(c, 40, 140, content_width,
              view->title[0] ? view->title : default_title(view), 3, TEXT);
    text_line(c, 40, 212, content_width, view->subtitle, 2, MUTED);

    const int footer_y = (int)height - 128;
    rect(c, 32, footer_y, (int)width - 64, 96, PANEL);
    rect(c, 32, footer_y, 4, 96, accent);
    text_wrap(c, 52, footer_y + 12, (int)width - 104, 76,
              view->footer[0] ? view->footer : default_footer(view->screen), 2, TEXT);

    if (view->screen == UI_NOTICE) {
        rect(c, 32, 300, (int)width - 64, footer_y - 324, PANEL);
        rect(c, 32, 300, 4, footer_y - 324, accent);
        text_line(c, 64, 332, (int)width - 128,
                  view->error ? "请检查后重试" : "操作提示", 2, accent);
        text_wrap(c, 64, 404, (int)width - 128, footer_y - 452,
                  view->message, 2, TEXT);
        return;
    }

    unsigned count = view->item_count < UI_VIEW_ROWS ? view->item_count : UI_VIEW_ROWS;
    char position[64];
    if (view->screen == UI_LIBRARY) {
        snprintf(position, sizeof(position), "%u - %u / %u",
                 count ? view->first + 1u : 0u, view->first + count, view->total);
        text_line(c, 40, 274, 300, position, 1, DIM);
    } else {
        text_line(c, 40, 274, content_width,
                  view->game_loaded ? "游戏已载入" : "请选择游戏", 1, DIM);
    }
    if (!count) {
        rect(c, 32, 320, (int)width - 64, footer_y - 344, PANEL);
        text_wrap(c, 64, 370, (int)width - 128, footer_y - 418,
                  view->message[0] ? view->message : "未找到游戏，请检查资源路径。", 2, MUTED);
        return;
    }

    for (unsigned i = 0; i < count; ++i) {
        const int y = 320 + (int)i * 96;
        int selected = i == view->selected;
        uint32_t foreground = selected ? INK : TEXT;
        uint32_t detail = selected ? INK : MUTED;
        rect(c, 32, y, (int)width - 64, 86, selected ? MINT : ROW);
        if (selected) rect(c, 32, y + 12, 4, 62, TEXT);
        if (view->directories[i]) {
            rect(c, 50, y + 31, 14, 5, foreground);
            rect(c, 50, y + 36, 22, 16, foreground);
            rect(c, 52, y + 38, 18, 12, selected ? MINT : ROW);
        } else {
            snprintf(position, sizeof(position), "%02u", view->first + i + 1u);
            text_line(c, 48, y + 33, 34, position, 1, detail);
        }
        text_line(c, 92, y + (view->details[i][0] ? 10 : 25),
                  (int)width - 176, view->items[i], 2, foreground);
        if (view->details[i][0])
            text_line(c, 92, y + 54, (int)width - 176, view->details[i], 1, detail);
        if (selected) text_line(c, (int)width - 70, y + 25, 24, ">", 2, INK);
        else rect(c, 48, y + 85, (int)width - 96, 1, LINE);
    }
}

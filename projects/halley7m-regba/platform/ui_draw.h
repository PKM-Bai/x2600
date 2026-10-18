/* X2600 ReGBA handheld menu renderer. GPL-2.0-or-later. */
#ifndef REGBA_UI_DRAW_H
#define REGBA_UI_DRAW_H

#include <stdint.h>
#include "game_stats.h"

#define UI_VIEW_ROWS 8

enum ui_screen { UI_LIBRARY, UI_PAUSE, UI_RESOURCES, UI_NOTICE, UI_SETTINGS };

typedef struct ui_view {
    enum ui_screen screen;
    char title[64], subtitle[256];
    char items[UI_VIEW_ROWS][256], details[UI_VIEW_ROWS][64];
    uint8_t directories[UI_VIEW_ROWS];
    /* selected is the zero-based visible row; first is its page's offset. */
    unsigned item_count, selected, total, first;
    char message[384], footer[192];
    int connected, game_loaded, error;
} ui_view_t;

/* Pure software, no allocation or hardware access. Context is a ui_view_t.
 * Pixels use tightly packed 0xFFRRGGBB, normally 720x1280. Unsupported or
 * malformed UTF-8 characters draw a visible replacement box. */
void ui_draw(uint32_t *pixels, unsigned width, unsigned height, void *context);

enum { UI_GAME_STATS_Y = 320, UI_GAME_STATS_HEIGHT = 64 };
/* Pure software HUD in the upper black border of the native game layout.
 * Only rows 320..383 change; the GBA area starts at row 400. Invalid samples
 * show placeholders until the first full window. No allocation/hardware. */
void ui_draw_game_stats(uint32_t *pixels, unsigned width, unsigned height,
                        const game_stats_t *stats);

#endif

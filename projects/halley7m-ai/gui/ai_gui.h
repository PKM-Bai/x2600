#ifndef AI_GUI_H
#define AI_GUI_H
#include "ai_assets.h"
#include "player_gui.h"
typedef int (*ai_gui_send_fn)(void *context, const char *utf8);
int ai_gui_init(const player_gui_output_t *backend, const ai_asset *font, const ai_asset *dictionary,
                const char *status, ai_gui_send_fn send, void *context);
int ai_gui_step(uint32_t now_ms, bool pressed, unsigned x, unsigned y, uint16_t keys);
void ai_gui_set_status(const char *text);
void ai_gui_font_set(const ai_asset *font);
/* GUI owner only, useful for local editing and production headless checks. */
const char *ai_gui_draft(void);
#endif

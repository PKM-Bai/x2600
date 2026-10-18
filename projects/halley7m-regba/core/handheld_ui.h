#ifndef REGBA_HANDHELD_UI_H
#define REGBA_HANDHELD_UI_H

#include "ui_draw.h"
#include <stddef.h>
#include <stdint.h>

enum ui_action {
    UI_ACTION_NONE, UI_ACTION_LAUNCH, UI_ACTION_RESUME, UI_ACTION_SAVE,
    UI_ACTION_SWITCH, UI_ACTION_EXIT, UI_ACTION_SOUND, UI_ACTION_REFRESH,
    UI_ACTION_RELOAD, UI_ACTION_VOLUME_DOWN, UI_ACTION_VOLUME_UP,
    UI_ACTION_FRAMESKIP_DOWN, UI_ACTION_FRAMESKIP_UP,
    UI_ACTION_SPEED_DOWN, UI_ACTION_SPEED_UP
};

/* All model and drawing calls belong to the emulator task. */
void handheld_ui_init(void);
void handheld_ui_library(int loaded);
void handheld_ui_pause(const char *game_path);
void handheld_ui_status(int connected, int loaded, int sound_enabled);
/* Read back applied settings without changing the selected row. The core
 * handles option actions and owns clamping/cycling and hardware changes. */
void handheld_ui_options(unsigned volume_percent, unsigned frameskip,
                         unsigned speed_multiplier);
void handheld_ui_notice(const char *title, const char *message, int error,
                        enum ui_action retry);
void handheld_ui_error(const char *operation, int result, enum ui_action retry);
enum ui_action handheld_ui_poll(uint16_t keys, int menu_pressed, uint32_t now_ms);
const char *handheld_ui_launch_path(void);
ui_view_t *handheld_ui_view(void);
int handheld_ui_dirty(void);
void handheld_ui_drawn(void);

#endif

#ifndef DISPLAY_HOST_PANEL_H
#define DISPLAY_HOST_PANEL_H
#include "x2600_hal.h"
extern struct lcd_data panel_zc502_data;
int panel_zc502_result(void);
void panel_zc502_backlight_on(void);
void panel_zc502_power_off(void);
#endif

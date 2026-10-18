/* HALLEY7-M ZC50289HSHD02 panel. GPL-2.0-or-later. */
#ifndef REGBA_PANEL_ZC502_H
#define REGBA_PANEL_ZC502_H

#include <x2600_hal_lcd.h>

extern struct lcd_data panel_zc502_data;
int panel_zc502_result(void);
void panel_zc502_backlight_on(void);
void panel_zc502_power_off(void);

#endif

#include "../../halley7m-player/gui/lv_conf.h"
#undef LV_MEM_SIZE
#define LV_MEM_SIZE (1024U * 1024U)
#undef LV_FONT_MONTSERRAT_14
#define LV_FONT_MONTSERRAT_14 1
#undef LV_FONT_DEFAULT
#define LV_FONT_DEFAULT &ai_font_32
#undef LV_FONT_CUSTOM_DECLARE
#define LV_FONT_CUSTOM_DECLARE extern const struct _lv_font_t ai_font_32;
#undef LV_USE_BTNMATRIX
#define LV_USE_BTNMATRIX 1
#undef LV_USE_TEXTAREA
#define LV_USE_TEXTAREA 1
#undef LV_USE_KEYBOARD
#define LV_USE_KEYBOARD 1
#undef LV_USE_BTN
#define LV_USE_BTN 1

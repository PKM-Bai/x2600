#ifndef X2600_REGBA_CORE_H
#define X2600_REGBA_CORE_H
#include <stdint.h>
int regba_core_start(void);
int regba_core_load(const char *path);
int regba_core_run(uint32_t frames);
int regba_core_selftest(void);
int regba_core_save(void);
/* Open the handheld library, or pause a running game at its next frame. */
int regba_core_menu(void);
int regba_core_tone(void);
/* 0=off, 1=on, 2=framebuffer test, 3=DSI internal test. Core task only. */
int regba_core_video(unsigned action);
void regba_core_stop(void);
void regba_core_keys(uint16_t keys);
void regba_core_audio(int enabled);
void regba_core_status(void);
int regba_jit_test(void);
#endif

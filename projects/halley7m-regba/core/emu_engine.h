/* X2600 emulator command facade. GPL-2.0-or-later. */
#ifndef X2600_EMU_ENGINE_H
#define X2600_EMU_ENGINE_H

#include <stdint.h>

/* Application-facing commands retain the existing asynchronous semantics:
 * load/run/menu/save requests are handled by the emulator task. A zero return
 * acknowledges a request, not completion. Backend execution is a separate,
 * synchronous contract and must not be called from the console task. */
int emu_engine_start(void);
/* The path is copied before this call returns; the caller retains ownership. */
int emu_engine_load(const char *path);
/* Zero frames runs until a stop/menu request or a core error. */
int emu_engine_run(uint32_t frames);
int emu_engine_selftest(void);
int emu_engine_save(void);
/* A menu pause settles an in-flight save without overwriting the disk save. */
int emu_engine_menu(void);
int emu_engine_tone(void);
/* 0=off, 1=on, 2=framebuffer test, 3=DSI internal test. */
int emu_engine_video(unsigned action);
/* These requests may arrive while the emulator task is inside run(). */
void emu_engine_stop(void);
void emu_engine_keys(uint16_t keys);
void emu_engine_audio(int enabled);

/* Immediate diagnostic snapshot, not an atomic transaction with the task. */
void emu_engine_status(void);
/* Synchronous CPU/JIT diagnostic; unlike the queued commands above. */
int emu_engine_jit_test(void);

#endif

/* X2600 emulator command facade. GPL-2.0-or-later. */
#include "emu_engine.h"
#include "regba_core.h"

/* Stage 1 preserves the existing owner/task and command implementation. */
int emu_engine_start(void) { return regba_core_start(); }
int emu_engine_load(const char *path) { return regba_core_load(path); }
int emu_engine_run(uint32_t frames) { return regba_core_run(frames); }
int emu_engine_selftest(void) { return regba_core_selftest(); }
int emu_engine_save(void) { return regba_core_save(); }
int emu_engine_menu(void) { return regba_core_menu(); }
int emu_engine_tone(void) { return regba_core_tone(); }
int emu_engine_video(unsigned action) { return regba_core_video(action); }
void emu_engine_stop(void) { regba_core_stop(); }
void emu_engine_keys(uint16_t keys) { regba_core_keys(keys); }
void emu_engine_audio(int enabled) { regba_core_audio(enabled); }
void emu_engine_status(void) { regba_core_status(); }
int emu_engine_jit_test(void) { return regba_jit_test(); }

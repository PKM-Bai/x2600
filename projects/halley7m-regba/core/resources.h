#ifndef REGBA_RESOURCES_H
#define REGBA_RESOURCES_H

#include <stddef.h>

#define REGBA_ROM_DIR "/GBA/ROMS"
#define REGBA_LEGACY_ROM_DIR "/gba_game"
#define REGBA_SAVE_DIR "/GBA/SAVES"
#define MGBA_SAVE_DIR "/GBA/SAVES/mGBA"
#define REGBA_CONFIG_DIR "/GBA/CONFIG"
#define REGBA_GAME_CONFIG_FILE "/GBA/CONFIG/game_config.txt"
#define REGBA_BUILTIN_GAME_CONFIG_FILE "/__game_config.txt"
#define REGBA_BIOS_FILE "/GBA/BIOS/gba_bios.bin"
#define REGBA_RESOURCE_PATH_MAX 512u

/* Standard/legacy ROM roots share existing basename.sav files. Other paths
 * append an ASCII-case-folded full-path CRC32 to distinguish subdirectories.
 * Returns 0 or negative errno; no truncation. Names retain their original case
 * (case variants address the same FatFs file). Internal save-test ROMs retain
 * their old names, while the core self-test has no battery file. */
int regba_backup_path(char *out, size_t capacity, const char *rom_path);

#endif

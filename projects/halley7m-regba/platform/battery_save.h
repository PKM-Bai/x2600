#ifndef REGBA_BATTERY_SAVE_H
#define REGBA_BATTERY_SAVE_H

#include <stddef.h>

/* Native GBA battery files only: 512 B, 8 KiB, 32 KiB, 64 KiB or 128 KiB.
 * The caller supplies an absolute TF path and prepares its directory first.
 * Results are 0 on success or negative errno; submit success means the owned
 * snapshot was queued, while wait/result report the completed disk write. */
int battery_save_start(void);
int battery_save_submit(const char *path, const void *data, size_t size);
int battery_save_busy(void);
int battery_save_result(void);
int battery_save_wait(void);
void battery_save_status(void);

/* Call only after flushing/stopping the previous game. A missing .sav can be
 * recovered from its previous completed .sav.bak; .tmp is never trusted. */
int battery_save_recover(const char *path);
int battery_save_load(const char *path, void *data, size_t capacity, size_t *size);

#ifdef BATTERY_SAVE_HOST_TEST
int battery_save_test_write(const char *path, const void *data, size_t size);
#endif
#endif

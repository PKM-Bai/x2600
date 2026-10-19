#ifndef PLAYER_STORAGE_H
#define PLAYER_STORAGE_H

#include "player_media.h"
#include <stdint.h>

struct player_storage_file;

/* Read-only SD-SPI/FAT12/16/32 adapter. The task that first calls mount owns
 * every operation, including parser IO callbacks and listing callbacks.
 * Calls are blocking, never from an ISR. No separate worker or mutex exists.
 * One file can be open at a time; close it before opening the next file.
 * The mount and its open stream remain valid until an IO failure or reset;
 * there is no card-detect GPIO or automatic hot-remount support.
 * All returned errors are negative errno values, except size() in media_io.
 */
int player_storage_mount(void);
int player_storage_open_utf8(struct player_storage_file **stream, const char *path);
int player_storage_close(struct player_storage_file *stream);
/* NULL for a closed/invalid stream. The returned adapter lives with stream. */
const struct player_media_io *player_storage_io(struct player_storage_file *stream);

/* UTF-8 entry name is valid only for the duration of the callback. Directory
 * sizes are reported as zero. A nonzero callback return stops the listing and
 * is propagated after closing the directory. No extension filter is applied.
 * Do not reenter storage from the callback.
 */
typedef int (*player_storage_visit_fn)(void *opaque, const char *name_utf8,
                                      uint64_t bytes, int is_directory);
int player_storage_ls_utf8(const char *path, player_storage_visit_fn visit, void *opaque);

#endif

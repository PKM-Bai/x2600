#ifndef PLAYER_STORAGE_H
#define PLAYER_STORAGE_H

#include "player_media.h"
#include <stdint.h>

struct player_storage_file;
struct player_storage_directory;
#define PLAYER_STORAGE_ENTRY_NAME_BYTES (255u * 4u + 1u)
typedef struct player_storage_entry {
    char name[PLAYER_STORAGE_ENTRY_NAME_BYTES];
    uint64_t bytes;
    int is_directory;
} player_storage_entry;

/* Read-only SD-SPI/FAT12/16/32 adapter. The task that first claims mount or
 * registers/opens the explicit embedded diagnostic owns
 * every operation, including parser IO callbacks and listing callbacks.
 * Calls are blocking, never from an ISR. No separate worker or mutex exists.
 * One file can be open at a time; close it before opening the next file.
 * The mount and its open stream remain valid until an IO failure or reset;
 * there is no card-detect GPIO or automatic hot-remount support.
 * All returned errors are negative errno values, except size() in media_io.
 */
int player_storage_mount(void);
int player_storage_open_utf8(struct player_storage_file **stream, const char *path);
typedef int (*player_storage_pump_fn)(void *opaque);
typedef struct player_storage_seek_stats {
    uint32_t enabled, capacity_entries, required_entries, used_entries;
    uint64_t build_us;
    uint32_t seek_calls;
    uint64_t max_seek_us;
} player_storage_seek_stats;
/* LOADING only, before audio/codec starts. >=1GiB TF files get a bounded,
 * malloc-owned 4096-DWORD CLMT. Smaller files and @embedded keep their path.
 * pump must not reenter storage; nonzero cancels. Build services pump and
 * yields one tick every 256 FAT steps. Insufficient map capacity is -E2BIG;
 * no silent slow-seek fallback. Close releases the map. */
int player_storage_prepare_seek(struct player_storage_file *stream,
                                player_storage_pump_fn pump, void *opaque);
/* Indexed containers also need bounded backward seeks below 1GiB. */
int player_storage_prepare_indexed_seek(struct player_storage_file *stream,
                                        player_storage_pump_fn pump, void *opaque);
/* Same owner; NULL stream returns the most recent file's retained stats.
 * Stats remain available after close until the next successful open. */
int player_storage_get_seek_stats(struct player_storage_file *stream,
                                  player_storage_seek_stats *out);
/* Explicit RAM diagnostic only. Register immutable firmware-linked bytes on
 * the storage worker before opening "@embedded". The bytes must outlive the
 * stream. That path shares the unique file slot and task owner but needs no
 * successful TF mount; it is never synthesized into directory listings. */
int player_storage_register_embedded(const void *bytes, uint32_t length);
/* Explicit CMD0/CMD8 GPIO transport diagnostic. Same worker only, and only
 * before a successful mount with no open file/directory. Never writes media
 * and never silently resets an already mounted card. */
int player_storage_gpio_probe(void);
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

/* Incremental directory cursor, owned by the same storage worker. One active
 * cursor may coexist with the one read-only file. next performs one FatFs
 * readdir and returns 0=entry, 1=end, negative=error. The caller schedules each
 * step between audio/video service; GUI tasks never call these functions. */
int player_storage_dir_open(struct player_storage_directory **directory, const char *path);
int player_storage_dir_next(struct player_storage_directory *directory, player_storage_entry *entry);
int player_storage_dir_close(struct player_storage_directory *directory);

#endif

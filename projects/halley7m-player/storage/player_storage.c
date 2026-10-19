#include "player_storage.h"
#include "sdspi.h"
#include "path_encoding.h"
#include "ff.h"
#include "FreeRTOS.h"
#include "task.h"
#include "platform_runtime.h"
#include <errno.h>
#include <limits.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#define PLAYER_STORAGE_PATH_UNITS 512u
#define PLAYER_STORAGE_NAME_BYTES (255u * 4u + 1u)
#define PLAYER_STORAGE_FILE_LIMIT UINT64_C(0x100000000)
#define PLAYER_STORAGE_MAP_MIN UINT64_C(0x40000000)
#define PLAYER_STORAGE_MAP_ENTRIES 4096u

_Static_assert(sizeof(FSIZE_t) == 4, "Player read-only FAT adapter expects 32-bit FSIZE_t");
_Static_assert(_FS_READONLY == 1 && _FS_EXFAT == 0 && _FS_REENTRANT == 0,
               "Player storage requires serialized read-only FAT without exFAT");
_Static_assert(_USE_FASTSEEK == 1 && sizeof(DWORD) == 4, "Player CLMT ABI");

struct player_storage_file {
    FIL fat_file;
    struct player_media_io io;
    uint64_t bytes;
    const uint8_t *embedded;
    uint32_t embedded_position;
    DWORD *linkmap;
    player_storage_seek_stats seek_stats;
    int opened;
};

static FATFS filesystem;
static TaskHandle_t owner;
static int mounted;
static struct player_storage_file media_file;
static const uint8_t *embedded_bytes;
static uint32_t embedded_length;
struct player_storage_directory {
    DIR fat_directory;
    int opened;
};
static struct player_storage_directory media_directory;
static const TCHAR drive[] = { '0', ':', 0 };
static struct {
    player_storage_pump_fn pump;
    void *opaque;
    uint32_t steps;
    int active, error;
} map_build;

static int map_pump(void)
{
    int result = map_build.pump ? map_build.pump(map_build.opaque) : 0;
    if (result) map_build.error = result < 0 ? result : -ECANCELED;
    return map_build.error;
}

/* Called only by the generated CREATE_LINKMAP inner FAT loop. Its counter
 * spans fragments: fully fragmented chains receive the same bounded pump. */
int player_storage_clmt_step(void)
{
    if (!map_build.active || (++map_build.steps & 255u)) return 0;
    if (map_pump()) return 1;
    vTaskDelay(1);
    return 0;
}

static int fat_error(FRESULT result)
{
    switch (result) {
    case FR_OK: return 0;
    case FR_NO_FILE: case FR_NO_PATH: return -ENOENT;
    case FR_INVALID_NAME: case FR_INVALID_PARAMETER: case FR_INVALID_DRIVE: return -EINVAL;
    case FR_INVALID_OBJECT: return -EBADF;
    case FR_DENIED: return -EACCES;
    case FR_WRITE_PROTECTED: return -EROFS;
    case FR_NOT_READY: case FR_NOT_ENABLED: case FR_NO_FILESYSTEM: return -ENODEV;
    case FR_TIMEOUT: return -ETIMEDOUT;
    case FR_LOCKED: return -EBUSY;
    case FR_NOT_ENOUGH_CORE: return -ENOMEM;
    case FR_TOO_MANY_OPEN_FILES: return -EMFILE;
    default: return -EIO;
    }
}

static int task_check(void)
{
    if (xPortIsInsideInterrupt() || xTaskGetSchedulerState() != taskSCHEDULER_RUNNING)
        return -EAGAIN;
    if (owner != xTaskGetCurrentTaskHandle()) return -EPERM;
    return 0;
}

static int owner_check(void)
{
    int result = task_check();
    if (result) return result;
    return mounted ? 0 : -ENODEV;
}

static int claim_owner(void)
{
    if (xPortIsInsideInterrupt() || xTaskGetSchedulerState() != taskSCHEDULER_RUNNING)
        return -EAGAIN;
    TaskHandle_t caller = xTaskGetCurrentTaskHandle();
    if (owner && owner != caller) return -EPERM;
    owner = caller;
    return 0;
}

static int stream_check(struct player_storage_file *stream)
{
    int result = task_check();
    if (result) return result;
    if (stream != &media_file || !stream->opened) return -EBADF;
    return stream->embedded || mounted ? 0 : -ENODEV;
}

static int32_t stream_read(void *opaque, void *buffer, uint32_t bytes)
{
    struct player_storage_file *stream = opaque;
    int result = stream_check(stream);
    if (result) return result;
    if (!buffer && bytes) return -EINVAL;
    if (bytes > INT32_MAX) return -EOVERFLOW;
    if (stream->embedded) {
        uint32_t remaining = (uint32_t)stream->bytes - stream->embedded_position;
        if (bytes > remaining) bytes = remaining;
        if (bytes) memcpy(buffer, stream->embedded + stream->embedded_position, bytes);
        stream->embedded_position += bytes;
        return (int32_t)bytes;
    }
    uint64_t position = f_tell(&stream->fat_file);
    if (position > stream->bytes) return -EIO;
    uint64_t remaining = stream->bytes - position;
    UINT request = (UINT)(remaining < bytes ? remaining : bytes);
    UINT received = 0;
    FRESULT fr = f_read(&stream->fat_file, buffer, request, &received);
    if (fr != FR_OK) return fat_error(fr);
    if (received > request) return -EIO;
    return (int32_t)received;
}

static int stream_seek(void *opaque, uint64_t absolute)
{
    struct player_storage_file *stream = opaque;
    int result = stream_check(stream);
    if (result) return result;
    /* Never truncate a 64-bit parser offset before checking the FAT API. */
    if (absolute >= PLAYER_STORAGE_FILE_LIMIT ||
        absolute > (uint64_t)(FSIZE_t)~(FSIZE_t)0 || absolute > stream->bytes)
        return -EOVERFLOW;
    if (stream->embedded) {
        stream->embedded_position = (uint32_t)absolute;
        return 0;
    }
    uint64_t started = platform_time_us();
    FRESULT fr;
    if (stream->linkmap && absolute == (uint64_t)CREATE_LINKMAP) {
        /* FatFs reserves UINT32_MAX for map construction. Keep the adapter's
         * <4GiB contract even for the last possible EOF offset. */
        uint8_t last;
        UINT received = 0;
        fr = f_lseek(&stream->fat_file, (FSIZE_t)(absolute - 1u));
        if (fr == FR_OK) fr = f_read(&stream->fat_file, &last, 1, &received);
        if (fr == FR_OK && received != 1) fr = FR_DISK_ERR;
    } else {
        fr = f_lseek(&stream->fat_file, (FSIZE_t)absolute);
    }
    ++stream->seek_stats.seek_calls;
    uint64_t elapsed = platform_time_us() - started;
    if (elapsed > stream->seek_stats.max_seek_us) stream->seek_stats.max_seek_us = elapsed;
    if (fr != FR_OK) return fat_error(fr);
    return (uint64_t)f_tell(&stream->fat_file) == absolute ? 0 : -EIO;
}

static uint64_t stream_size(void *opaque)
{
    struct player_storage_file *stream = opaque;
    return stream_check(stream) == 0 ? stream->bytes : 0;
}

int player_storage_mount(void)
{
    int result = claim_owner();
    if (result) return result;
    if (mounted) return 0;
    result = fat_error(f_mount(&filesystem, drive, 1));
    mounted = result == 0;
    return result;
}

int player_storage_register_embedded(const void *bytes, uint32_t length)
{
    int result;
    if (!bytes || !length) return -EINVAL;
    result = claim_owner();
    if (result) return result;
    if (media_file.opened) return -EBUSY;
    embedded_bytes = bytes;
    embedded_length = length;
    return 0;
}

int player_storage_gpio_probe(void)
{
    int result = claim_owner();
    if (result) return result;
    if (mounted || media_file.opened || media_directory.opened) return -EBUSY;
    return sdspi_gpio_probe();
}

int player_storage_open_utf8(struct player_storage_file **stream, const char *path)
{
    if (stream) *stream = NULL;
    if (!stream || !path) return -EINVAL;
    int diagnostic = !strcmp(path, "@embedded");
    int result = diagnostic ? claim_owner() : owner_check();
    if (result) return result;
    if (media_file.opened) return -EBUSY;
    if (diagnostic) {
        if (!embedded_bytes) return -ENOENT;
        media_file.bytes = embedded_length;
        media_file.embedded = embedded_bytes;
        media_file.embedded_position = 0;
        media_file.linkmap = NULL;
        memset(&media_file.seek_stats, 0, sizeof(media_file.seek_stats));
        media_file.io = (struct player_media_io){ &media_file, stream_read, stream_seek, stream_size };
        media_file.opened = 1;
        *stream = &media_file;
        return 0;
    }
    TCHAR fat_path[PLAYER_STORAGE_PATH_UNITS];
    result = utf8_to_utf16(fat_path, PLAYER_STORAGE_PATH_UNITS, path);
    if (result) return result;
    FRESULT fr = f_open(&media_file.fat_file, fat_path, FA_READ);
    if (fr != FR_OK) return fat_error(fr);
    media_file.bytes = f_size(&media_file.fat_file);
    if (media_file.bytes >= PLAYER_STORAGE_FILE_LIMIT ||
        media_file.bytes > (uint64_t)(FSIZE_t)~(FSIZE_t)0) {
        (void)f_close(&media_file.fat_file);
        return -EFBIG;
    }
    media_file.io = (struct player_media_io){ &media_file, stream_read, stream_seek, stream_size };
    media_file.embedded = NULL;
    media_file.linkmap = NULL;
    memset(&media_file.seek_stats, 0, sizeof(media_file.seek_stats));
    media_file.opened = 1;
    *stream = &media_file;
    return 0;
}

static int prepare_seek(struct player_storage_file *stream,
                         player_storage_pump_fn pump, void *opaque, unsigned indexed)
{
    int result = stream_check(stream);
    if (result) return result;
    if (stream->embedded || (!indexed && stream->bytes < PLAYER_STORAGE_MAP_MIN) || stream->linkmap) return 0;
    if (!pump) return -EINVAL;
    stream->seek_stats.enabled = stream->seek_stats.required_entries = stream->seek_stats.used_entries = 0;
    stream->seek_stats.build_us = 0;
    stream->seek_stats.capacity_entries = PLAYER_STORAGE_MAP_ENTRIES;
    stream->linkmap = malloc(PLAYER_STORAGE_MAP_ENTRIES * sizeof(DWORD));
    if (!stream->linkmap) return -ENOMEM;
    stream->linkmap[0] = PLAYER_STORAGE_MAP_ENTRIES;
    stream->fat_file.cltbl = stream->linkmap;
    memset(&map_build, 0, sizeof(map_build));
    map_build.pump = pump;
    map_build.opaque = opaque;
    map_build.active = 1;
    uint64_t started = platform_time_us();
    FRESULT fr = FR_OK;
    if (!map_pump()) fr = f_lseek(&stream->fat_file, CREATE_LINKMAP);
    if (!map_build.error) (void)map_pump();
    stream->seek_stats.build_us = platform_time_us() - started;
    map_build.active = 0;
    if ((fr == FR_OK && !map_build.error) || fr == FR_NOT_ENOUGH_CORE)
        stream->seek_stats.required_entries = stream->linkmap[0];
    result = map_build.error ? map_build.error : fr == FR_NOT_ENOUGH_CORE ? -E2BIG : fat_error(fr);
    if (result) {
        stream->fat_file.cltbl = NULL;
        free(stream->linkmap);
        stream->linkmap = NULL;
        return result;
    }
    stream->seek_stats.used_entries = stream->linkmap[0];
    stream->seek_stats.enabled = 1;
    return 0;
}

int player_storage_prepare_seek(struct player_storage_file *stream,
                                player_storage_pump_fn pump, void *opaque)
{
    return prepare_seek(stream, pump, opaque, 0);
}

int player_storage_prepare_indexed_seek(struct player_storage_file *stream,
                                        player_storage_pump_fn pump, void *opaque)
{
    return prepare_seek(stream, pump, opaque, 1);
}

int player_storage_get_seek_stats(struct player_storage_file *stream, player_storage_seek_stats *out)
{
    int result = task_check();
    if (result) return result;
    if (!out || (stream && stream != &media_file)) return -EINVAL;
    *out = media_file.seek_stats;
    return 0;
}

int player_storage_close(struct player_storage_file *stream)
{
    int result = stream_check(stream);
    if (result) return result;
    FRESULT fr = stream->embedded ? FR_OK : f_close(&stream->fat_file);
    stream->fat_file.cltbl = NULL;
    free(stream->linkmap);
    stream->linkmap = NULL;
    /* Do not retain a failed stale FIL as a usable parser stream. */
    stream->opened = 0;
    stream->bytes = 0;
    stream->embedded = NULL;
    stream->embedded_position = 0;
    return fat_error(fr);
}

const struct player_media_io *player_storage_io(struct player_storage_file *stream)
{
    return stream_check(stream) == 0 ? &stream->io : NULL;
}

int player_storage_dir_open(struct player_storage_directory **directory, const char *path)
{
    TCHAR fat_path[PLAYER_STORAGE_PATH_UNITS];
    int result;
    if (directory) *directory = NULL;
    if (!directory || !path) return -EINVAL;
    result = owner_check();
    if (result) return result;
    if (media_directory.opened) return -EBUSY;
    result = utf8_to_utf16(fat_path, PLAYER_STORAGE_PATH_UNITS, path);
    if (result) return result;
    result = fat_error(f_opendir(&media_directory.fat_directory, fat_path));
    if (result) return result;
    media_directory.opened = 1;
    *directory = &media_directory;
    return 0;
}

int player_storage_dir_next(struct player_storage_directory *directory, player_storage_entry *entry)
{
    FILINFO information;
    int result = owner_check();
    if (result) return result;
    if (directory != &media_directory || !directory->opened) return -EBADF;
    if (!entry) return -EINVAL;
    result = fat_error(f_readdir(&directory->fat_directory, &information));
    if (result) return result;
    if (!information.fname[0]) return 1;
    result = utf16_to_utf8(entry->name, sizeof(entry->name), information.fname);
    if (result) return result;
    entry->is_directory = (information.fattrib & AM_DIR) != 0;
    entry->bytes = entry->is_directory ? 0 : (uint64_t)information.fsize;
    return 0;
}

int player_storage_dir_close(struct player_storage_directory *directory)
{
    int result = owner_check();
    if (result) return result;
    if (directory != &media_directory || !directory->opened) return -EBADF;
    result = fat_error(f_closedir(&directory->fat_directory));
    directory->opened = 0;
    return result;
}

int player_storage_ls_utf8(const char *path, player_storage_visit_fn visit, void *opaque)
{
    if (!path || !visit) return -EINVAL;
    int result = owner_check();
    if (result) return result;
    TCHAR fat_path[PLAYER_STORAGE_PATH_UNITS];
    result = utf8_to_utf16(fat_path, PLAYER_STORAGE_PATH_UNITS, path);
    if (result) return result;
    DIR directory;
    FRESULT fr = f_opendir(&directory, fat_path);
    if (fr != FR_OK) return fat_error(fr);
    for (;;) {
        FILINFO entry;
        fr = f_readdir(&directory, &entry);
        if (fr != FR_OK) { result = fat_error(fr); break; }
        if (!entry.fname[0]) break;
        char name[PLAYER_STORAGE_NAME_BYTES];
        result = utf16_to_utf8(name, sizeof(name), entry.fname);
        if (result) break;
        int is_directory = (entry.fattrib & AM_DIR) != 0;
        result = visit(opaque, name, is_directory ? 0 : (uint64_t)entry.fsize, is_directory);
        if (result) break;
    }
    fr = f_closedir(&directory);
    return result ? result : fat_error(fr);
}

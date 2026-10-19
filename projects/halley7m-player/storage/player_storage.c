#include "player_storage.h"
#include "path_encoding.h"
#include "ff.h"
#include "FreeRTOS.h"
#include "task.h"
#include <errno.h>
#include <limits.h>
#include <stddef.h>

#define PLAYER_STORAGE_PATH_UNITS 512u
#define PLAYER_STORAGE_NAME_BYTES (255u * 4u + 1u)
#define PLAYER_STORAGE_FILE_LIMIT UINT64_C(0x100000000)

_Static_assert(sizeof(FSIZE_t) == 4, "Player read-only FAT adapter expects 32-bit FSIZE_t");
_Static_assert(_FS_READONLY == 1 && _FS_EXFAT == 0 && _FS_REENTRANT == 0,
               "Player storage requires serialized read-only FAT without exFAT");

struct player_storage_file {
    FIL fat_file;
    struct player_media_io io;
    uint64_t bytes;
    int opened;
};

static FATFS filesystem;
static TaskHandle_t owner;
static int mounted;
static struct player_storage_file media_file;
static const TCHAR drive[] = { '0', ':', 0 };

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

static int owner_check(void)
{
    if (xPortIsInsideInterrupt() || xTaskGetSchedulerState() != taskSCHEDULER_RUNNING)
        return -EAGAIN;
    if (owner != xTaskGetCurrentTaskHandle()) return -EPERM;
    return mounted ? 0 : -ENODEV;
}

static int stream_check(struct player_storage_file *stream)
{
    int result = owner_check();
    if (result) return result;
    return stream == &media_file && stream->opened ? 0 : -EBADF;
}

static int32_t stream_read(void *opaque, void *buffer, uint32_t bytes)
{
    struct player_storage_file *stream = opaque;
    int result = stream_check(stream);
    if (result) return result;
    if (!buffer && bytes) return -EINVAL;
    if (bytes > INT32_MAX) return -EOVERFLOW;
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
    FRESULT fr = f_lseek(&stream->fat_file, (FSIZE_t)absolute);
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
    if (xPortIsInsideInterrupt() || xTaskGetSchedulerState() != taskSCHEDULER_RUNNING)
        return -EAGAIN;
    TaskHandle_t caller = xTaskGetCurrentTaskHandle();
    if (owner && owner != caller) return -EPERM;
    owner = caller;
    if (mounted) return 0;
    int result = fat_error(f_mount(&filesystem, drive, 1));
    mounted = result == 0;
    return result;
}

int player_storage_open_utf8(struct player_storage_file **stream, const char *path)
{
    if (stream) *stream = NULL;
    if (!stream || !path) return -EINVAL;
    int result = owner_check();
    if (result) return result;
    if (media_file.opened) return -EBUSY;
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
    media_file.opened = 1;
    *stream = &media_file;
    return 0;
}

int player_storage_close(struct player_storage_file *stream)
{
    int result = stream_check(stream);
    if (result) return result;
    FRESULT fr = f_close(&stream->fat_file);
    /* Do not retain a failed stale FIL as a usable parser stream. */
    stream->opened = 0;
    stream->bytes = 0;
    return fat_error(fr);
}

const struct player_media_io *player_storage_io(struct player_storage_file *stream)
{
    return stream_check(stream) == 0 ? &stream->io : NULL;
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

/* Single-owner FatFs task. Synchronous calls keep caller buffers alive until
 * completion. UART descriptors are handled outside this module. */
#include "storage.h"
#include "sdspi.h"
#include "fatfs/ff.h"
#include "path_encoding.h"
#ifndef STORAGE_SCAN_HOST_TEST
#include "FreeRTOS.h"
#include "task.h"
#include "queue.h"
#include "semphr.h"
#endif
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>

enum { PATH_CAPACITY = 513, NAME_CAPACITY = 256 };

/* Application paths are UTF-8 with at most 512 bytes. FatFs 68300 uses
 * 16-bit TCHAR here; native L"..." literals are not portable to MIPS. */
static int convert_path(TCHAR out[PATH_CAPACITY], const char *path)
{
    if (!path) return -EINVAL;
    for (size_t length = 0; length < PATH_CAPACITY; ++length)
        if (!path[length]) return utf8_to_utf16(out, PATH_CAPACITY, path);
    return -ENAMETOOLONG;
}

#ifndef STORAGE_SCAN_HOST_TEST
#define FILE_SLOTS 8
typedef struct {
    FIL file;
    uint32_t position;
    int flags, used;
} OpenFile;
typedef enum {
    FS_MOUNT, FS_PREPARE, FS_LIST, FS_CRC, FS_TEST, FS_OPEN, FS_CLOSE,
    FS_READ, FS_WRITE, FS_SEEK, FS_FSTAT, FS_STAT, FS_SYNC, FS_UNLINK, FS_RENAME,
    FS_TF_PROBE, FS_SCAN_GBA
} Operation;
typedef struct {
    Operation operation;
    const char *path, *new_path;
    void *buffer;
    size_t size;
    int fd, flags, mode, whence;
    long offset, result;
    uint32_t *crc, *bytes;
    uint32_t scan_offset, *total;
    size_t *count;
} Request;

static FATFS volume;
static OpenFile files[FILE_SLOTS];
static QueueHandle_t requests;
static SemaphoreHandle_t rpc_lock, completed;
static TaskHandle_t owner;
static int mounted;
#endif

static int error(FRESULT result)
{
    switch (result) {
    case FR_OK: return 0;
    case FR_NO_FILE: case FR_NO_PATH: return -ENOENT;
    case FR_INVALID_NAME: case FR_INVALID_PARAMETER: return -EINVAL;
    case FR_DENIED: return -EACCES;
    case FR_EXIST: return -EEXIST;
    case FR_INVALID_OBJECT: return -EBADF;
    case FR_WRITE_PROTECTED: return -EROFS;
    case FR_INVALID_DRIVE: case FR_NOT_READY: return -ENODEV;
    case FR_NOT_ENABLED: return -ENXIO;
    case FR_NO_FILESYSTEM: return -ENODEV;
    case FR_TIMEOUT: return -ETIMEDOUT;
    case FR_LOCKED: return -EBUSY;
    case FR_NOT_ENOUGH_CORE: return -ENOMEM;
    case FR_TOO_MANY_OPEN_FILES: return -EMFILE;
    default: return -EIO;
    }
}

static int scan_gba_directory(const char *path, uint32_t offset,
                               storage_entry_t *entries, size_t capacity,
                               uint32_t *total, size_t *count)
{
    DIR dir;
    FILINFO info;
    uint32_t found = 0;
    size_t copied = 0;
    FRESULT result;
    TCHAR path16[PATH_CAPACITY];
    char name[NAME_CAPACITY];
    int conversion_error;
    if (total) *total = 0;
    if (count) *count = 0;
    if (!path || !path[0] || !total || !count || (!entries && capacity))
        return -EINVAL;
    conversion_error = convert_path(path16, path);
    if (conversion_error < 0) return conversion_error;
    result = f_opendir(&dir, path16);
    if (result != FR_OK) return error(result);
    while ((result = f_readdir(&dir, &info)) == FR_OK && info.fname[0]) {
        conversion_error = utf16_to_utf8(name, sizeof(name), info.fname);
        if (conversion_error < 0) break;
        size_t length = strlen(name);
        int directory = !!(info.fattrib & AM_DIR);
        if ((info.fattrib & (AM_HID | AM_SYS)) ||
            !strcmp(name, ".") || !strcmp(name, "..")) continue;
        if (!directory && (length < 4 || name[length - 4] != '.' ||
            (name[length - 3] != 'g' && name[length - 3] != 'G') ||
            (name[length - 2] != 'b' && name[length - 2] != 'B') ||
            (name[length - 1] != 'a' && name[length - 1] != 'A'))) continue;
        if (found >= offset && copied < capacity) {
            storage_entry_t *entry = entries + copied++;
            memcpy(entry->name, name, length + 1u);
            entry->size = info.fsize;
            entry->directory = (uint8_t)directory;
        }
        ++found;
    }
    FRESULT close_result = f_closedir(&dir);
    if (conversion_error < 0) return conversion_error;
    if (result != FR_OK) return error(result);
    if (close_result != FR_OK) return error(close_result);
    *total = found;
    *count = copied;
    return 0;
}

#ifndef STORAGE_SCAN_HOST_TEST
static OpenFile *file_for(int fd)
{
    if (fd < 3 || fd >= 3 + FILE_SLOTS || !files[fd - 3].used) return NULL;
    return &files[fd - 3];
}

static int prepare_dirs(void)
{
    static const char *paths[] = {
        "/GBA", "/GBA/ROMS", "/GBA/SAVES", "/GBA/BIOS", "/GBA/CONFIG"
    };
    TCHAR path16[PATH_CAPACITY];
    for (unsigned i = 0; i < sizeof(paths) / sizeof(paths[0]); i++) {
        int converted = convert_path(path16, paths[i]);
        if (converted < 0) return converted;
        FRESULT result = f_mkdir(path16);
        if (result != FR_OK && result != FR_EXIST) return error(result);
        FILINFO info;
        result = f_stat(path16, &info);
        if (result != FR_OK) return error(result);
        if (!(info.fattrib & AM_DIR)) return -ENOTDIR;
    }
    return 0;
}

static int list_directory(const char *path)
{
    DIR dir;
    FILINFO info;
    TCHAR path16[PATH_CAPACITY];
    char name[NAME_CAPACITY];
    int conversion_error = convert_path(path16, path);
    if (conversion_error < 0) return conversion_error;
    FRESULT result = f_opendir(&dir, path16);
    if (result != FR_OK) return error(result);
    while ((result = f_readdir(&dir, &info)) == FR_OK && info.fname[0]) {
        conversion_error = utf16_to_utf8(name, sizeof(name), info.fname);
        if (conversion_error < 0) break;
        printf("%c %10lu %s\n", info.fattrib & AM_DIR ? 'd' : '-',
               (unsigned long)info.fsize, name);
    }
    FRESULT close_result = f_closedir(&dir);
    if (conversion_error < 0) return conversion_error;
    return error(result != FR_OK ? result : close_result);
}

static uint32_t update_crc(uint32_t crc, const uint8_t *data, size_t size)
{
    while (size--) {
        crc ^= *data++;
        for (unsigned n = 0; n < 8; n++)
            crc = (crc >> 1) ^ ((crc & 1) ? 0xedb88320u : 0);
    }
    return crc;
}

static void test_error(const char *label, const char *phase, const char *path,
                       int fr, int result, uint32_t bytes, UINT moved, int close_fr)
{
    /* -1 means the raw result is unavailable or close was not attempted. */
    printf("%s phase=%s path=%s fr=%d result=%d bytes=%lu moved=%u close_fr=%d\n",
           label, phase, path, fr, result, (unsigned long)bytes,
           (unsigned)moved, close_fr);
}

static int crc_file(const char *path, uint32_t *crc_out, uint32_t *size_out)
{
    FIL file;
    TCHAR path16[PATH_CAPACITY];
    uint8_t data[4096];
    uint32_t crc = UINT32_MAX, size = 0;
    UINT got = 0;
    int converted = convert_path(path16, path);
    if (converted < 0) return converted;
    FRESULT result = f_open(&file, path16, FA_READ);
    if (result != FR_OK) {
        test_error("FS_CRC", "open", path, result, error(result), 0, 0, -1);
        return error(result);
    }
    do {
        result = f_read(&file, data, sizeof(data), &got);
        if (result != FR_OK) break;
        crc = update_crc(crc, data, got);
        size += got;
    } while (got);
    FRESULT close_result = f_close(&file);
    if (result != FR_OK || close_result != FR_OK) {
        FRESULT failure = result != FR_OK ? result : close_result;
        test_error("FS_CRC", result != FR_OK ? "read" : "close", path,
                   failure, error(failure), size, got, close_result);
        return error(result != FR_OK ? result : close_result);
    }
    *crc_out = crc ^ UINT32_MAX;
    *size_out = size;
    return 0;
}

static uint8_t test_byte(uint32_t offset)
{
    return (uint8_t)((offset * 37u) ^ (offset >> 8) ^ 0xa5u);
}

static int write_test(void)
{
    /* Create a new file only. Existing files are never overwritten/deleted. */
    int result = prepare_dirs();
    if (result < 0) {
        test_error("FS_TEST", "prepare", "/GBA", -1, result, 0, 0, -1);
        return result;
    }
    TCHAR path16[PATH_CAPACITY];
    result = convert_path(path16, "/GBA/TEST");
    if (result < 0) return result;
    FRESULT fr = f_mkdir(path16);
    if (fr != FR_OK && fr != FR_EXIST) {
        test_error("FS_TEST", "prepare-test-dir", "/GBA/TEST", fr,
                   error(fr), 0, 0, -1);
        return error(fr);
    }
    char path[40];
    FIL file;
    unsigned index;
    for (index = 0; index < 100; index++) {
        snprintf(path, sizeof(path), "/GBA/TEST/SDTEST%02u.BIN", index);
        result = convert_path(path16, path);
        if (result < 0) return result;
        fr = f_open(&file, path16, FA_WRITE | FA_CREATE_NEW);
        if (fr != FR_EXIST) break;
    }
    if (index == 100) {
        test_error("FS_TEST", "open", path, fr, -EEXIST, 0, 0, -1);
        return -EEXIST;
    }
    if (fr != FR_OK) {
        test_error("FS_TEST", "open", path, fr, error(fr), 0, 0, -1);
        return error(fr);
    }
    uint8_t data[4096];
    UINT moved = 0;
    uint32_t expected_crc = UINT32_MAX;
    uint32_t processed = 0;
    const char *failure_phase = NULL;
    FRESULT failure_fr = FR_OK;
    for (uint32_t offset = 0; offset < 128u * 1024; offset += sizeof(data)) {
        for (unsigned n = 0; n < sizeof(data); n++) data[n] = test_byte(offset + n);
        expected_crc = update_crc(expected_crc, data, sizeof(data));
        fr = f_write(&file, data, sizeof(data), &moved);
        if (fr != FR_OK || moved != sizeof(data)) {
            result = fr != FR_OK ? error(fr) : -ENOSPC;
            failure_phase = "write";
            failure_fr = fr;
            break;
        }
        processed += moved;
    }
    if (result == 0) {
        fr = f_sync(&file);
        result = error(fr);
        if (result < 0) {
            failure_phase = "sync";
            failure_fr = fr;
        }
    }
    fr = f_close(&file);
    if (result == 0) {
        result = error(fr);
        if (result < 0) {
            failure_phase = "close";
            failure_fr = fr;
        }
    }
    if (result < 0) {
        test_error("FS_TEST", failure_phase, path, failure_fr, result,
                   processed, moved, fr);
        return result;
    }
    uint32_t crc, bytes;
    result = crc_file(path, &crc, &bytes);
    if (result < 0) {
        /* FS_CRC above identifies open (reopen here), read, or close. */
        test_error("FS_TEST", "readback", path, -1, result, processed, 0, fr);
        return result;
    }
    if (bytes != 128u * 1024 || crc != (expected_crc ^ UINT32_MAX)) {
        printf("FS_TEST phase=crc-mismatch path=%s fr=%d result=%d bytes=%lu "
               "moved=0 close_fr=%d expected_bytes=131072 crc32=%08lx expected_crc32=%08lx\n",
               path, FR_OK, -EILSEQ, (unsigned long)bytes, fr,
               (unsigned long)crc, (unsigned long)(expected_crc ^ UINT32_MAX));
        return -EILSEQ;
    }
    printf("TF test OK: %s bytes=%lu crc32=%08lx; keep file for reboot readback\n",
           path, (unsigned long)bytes, (unsigned long)crc);
    return 0;
}

static int open_file(const char *path, int flags)
{
    BYTE access;
    int slot;
    TCHAR path16[PATH_CAPACITY];
    int converted = convert_path(path16, path);
    if (converted < 0) return converted;
    switch (flags & O_ACCMODE) {
    case O_RDONLY: access = FA_READ; break;
    case O_WRONLY: access = FA_WRITE; break;
    case O_RDWR: access = FA_READ | FA_WRITE; break;
    default: return -EINVAL;
    }
    if ((flags & (O_TRUNC | O_APPEND)) && !(access & FA_WRITE)) return -EINVAL;
    for (slot = 0; slot < FILE_SLOTS && files[slot].used; slot++);
    if (slot == FILE_SLOTS) return -EMFILE;
    if (flags & O_CREAT)
        access |= (flags & O_EXCL) ? FA_CREATE_NEW :
                  ((flags & O_TRUNC) ? FA_CREATE_ALWAYS : FA_OPEN_ALWAYS);
    FRESULT result = f_open(&files[slot].file, path16, access);
    if (result != FR_OK) return error(result);
    if ((flags & O_TRUNC) && !(flags & O_CREAT)) {
        result = f_truncate(&files[slot].file);
        if (result != FR_OK) { f_close(&files[slot].file); return error(result); }
    }
    files[slot].flags = flags;
    files[slot].position = (flags & O_APPEND) ? f_size(&files[slot].file) : 0;
    files[slot].used = 1;
    return slot + 3;
}

static int read_file(OpenFile *f, void *buffer, size_t size)
{
    if ((f->flags & O_ACCMODE) == O_WRONLY) return -EBADF;
    if (size > INT_MAX) return -EINVAL;
    if (f->position >= f_size(&f->file)) return 0;
    FRESULT result = f_lseek(&f->file, f->position);
    if (result != FR_OK) return error(result);
    UINT got;
    result = f_read(&f->file, buffer, size, &got);
    f->position += got;
    return got ? (int)got : error(result);
}

static int write_file(OpenFile *f, const void *buffer, size_t size)
{
    if ((f->flags & O_ACCMODE) == O_RDONLY) return -EBADF;
    if (size > INT_MAX || size > INT_MAX - f->position) return -EFBIG;
    if (f->flags & O_APPEND) f->position = f_size(&f->file);
    if (!size) return 0;
    /* POSIX seek past EOF: fill the gap, don't leak old card contents. */
    if (f->position > f_size(&f->file)) {
        uint8_t zeros[512] = {0};
        FRESULT result = f_lseek(&f->file, f_size(&f->file));
        if (result != FR_OK) return error(result);
        while (f_tell(&f->file) < f->position) {
            UINT amount = f->position - f_tell(&f->file), wrote;
            if (amount > sizeof(zeros)) amount = sizeof(zeros);
            result = f_write(&f->file, zeros, amount, &wrote);
            if (result != FR_OK || wrote != amount)
                return result != FR_OK ? error(result) : -ENOSPC;
        }
    }
    FRESULT result = f_lseek(&f->file, f->position);
    if (result != FR_OK) return error(result);
    UINT wrote;
    result = f_write(&f->file, buffer, size, &wrote);
    f->position += wrote;
    return wrote ? (int)wrote : result == FR_OK ? -ENOSPC : error(result);
}

static void fill_stat(struct stat *st, uint32_t size, int directory, int readonly)
{
    memset(st, 0, sizeof(*st));
    st->st_mode = (directory ? S_IFDIR : S_IFREG) | (readonly ? 0444 : 0666);
    st->st_nlink = 1;
    st->st_size = size;
    st->st_blksize = 512;
    st->st_blocks = (size + 511u) / 512u;
}

static long dispatch(Request *r)
{
    OpenFile *f;
    FRESULT result;
    if (r->operation == FS_TF_PROBE) {
        if (mounted) return -EBUSY;
        for (unsigned i = 0; i < FILE_SLOTS; i++) if (files[i].used) return -EBUSY;
        return sdspi_gpio_probe();
    }
    if (r->operation == FS_MOUNT) {
        static const TCHAR root_path[] = {0};
        for (unsigned i = 0; i < FILE_SLOTS; i++) if (files[i].used) return -EBUSY;
        mounted = 0;
        result = f_mount(&volume, root_path, 1);
        if (result != FR_OK) {
            printf("TF mount failed: FatFs=%u\n", (unsigned)result);
            return error(result);
        }
        if (volume.fs_type != FS_FAT32) { f_mount(NULL, root_path, 0); return -ENOTSUP; }
        mounted = 1;
        printf("TF FAT32 mounted: sectors=%lu (%lu MiB), cluster=%u sectors\n",
               (unsigned long)sdspi_sector_count(),
               (unsigned long)(sdspi_sector_count() / 2048), volume.csize);
        return 0;
    }
    if (!mounted) return -ENODEV;
    switch (r->operation) {
    case FS_PREPARE: return prepare_dirs();
    case FS_LIST: return list_directory(r->path);
    case FS_SCAN_GBA:
        return scan_gba_directory(r->path, r->scan_offset, r->buffer,
                                  r->size, r->total, r->count);
    case FS_CRC: return crc_file(r->path, r->crc, r->bytes);
    case FS_TEST: return write_test();
    case FS_OPEN: return open_file(r->path, r->flags);
    case FS_STAT: {
        FILINFO info;
        TCHAR path16[PATH_CAPACITY];
        int converted = convert_path(path16, r->path);
        if (converted < 0) return converted;
        result = f_stat(path16, &info);
        if (result != FR_OK) return error(result);
        fill_stat(r->buffer, info.fsize, !!(info.fattrib & AM_DIR), !!(info.fattrib & AM_RDO));
        return 0;
    }
    case FS_UNLINK: {
        TCHAR path16[PATH_CAPACITY];
        int converted = convert_path(path16, r->path);
        if (converted < 0) return converted;
        return error(f_unlink(path16));
    }
    case FS_RENAME: {
        TCHAR old_path16[PATH_CAPACITY], new_path16[PATH_CAPACITY];
        int converted = convert_path(old_path16, r->path);
        if (converted < 0) return converted;
        converted = convert_path(new_path16, r->new_path);
        if (converted < 0) return converted;
        return error(f_rename(old_path16, new_path16));
    }
    default: break;
    }
    f = file_for(r->fd);
    if (!f) return -EBADF;
    switch (r->operation) {
    case FS_CLOSE:
        result = f_close(&f->file);
        if (result == FR_OK) f->used = 0;
        return error(result);
    case FS_READ: return read_file(f, r->buffer, r->size);
    case FS_WRITE: return write_file(f, r->buffer, r->size);
    case FS_SYNC: return error(f_sync(&f->file));
    case FS_FSTAT:
        fill_stat(r->buffer, f_size(&f->file), 0, 0);
        return 0;
    case FS_SEEK: {
        int64_t base;
        if (r->whence == SEEK_SET) base = 0;
        else if (r->whence == SEEK_CUR) base = f->position;
        else if (r->whence == SEEK_END) base = f_size(&f->file);
        else return -EINVAL;
        int64_t position = base + r->offset;
        if (position < 0) return -EINVAL;
        if (position > INT_MAX) return -EOVERFLOW;
        f->position = (uint32_t)position;
        return (long)position;
    }
    default: return -EINVAL;
    }
}

static void filesystem_task(void *unused)
{
    (void)unused;
    Request *request;
    for (;;) {
        if (xQueueReceive(requests, &request, portMAX_DELAY) == pdTRUE) {
            request->result = dispatch(request);
            xSemaphoreGive(completed);
        }
    }
}

int storage_start(void)
{
    /* Called once during application task startup. */
    if (owner) return 0;
    requests = xQueueCreate(1, sizeof(Request *));
    rpc_lock = xSemaphoreCreateMutex();
    completed = xSemaphoreCreateBinary();
    if (!requests || !rpc_lock || !completed) goto fail;
    if (xTaskCreate(filesystem_task, "filesystem", 4096, NULL, 3, &owner) != pdPASS)
        goto fail;
    return 0;
fail:
    if (requests) vQueueDelete(requests);
    if (rpc_lock) vSemaphoreDelete(rpc_lock);
    if (completed) vSemaphoreDelete(completed);
    requests = NULL; rpc_lock = NULL; completed = NULL;
    return -ENOMEM;
}

static long call(Request *request)
{
    if (!owner || xTaskGetSchedulerState() != taskSCHEDULER_RUNNING) return -ENXIO;
    if (xTaskGetCurrentTaskHandle() == owner) return -EDEADLK;
    xSemaphoreTake(rpc_lock, portMAX_DELAY);
    xQueueSend(requests, &request, portMAX_DELAY);
    xSemaphoreTake(completed, portMAX_DELAY);
    long result = request->result;
    xSemaphoreGive(rpc_lock);
    return result;
}

int storage_mount(void) { Request r = {.operation = FS_MOUNT}; return call(&r); }
int storage_prepare_dirs(void) { Request r = {.operation = FS_PREPARE}; return call(&r); }
int storage_list(const char *path)
{ if (!path) return -EINVAL; Request r = {.operation = FS_LIST, .path = path}; return call(&r); }
int storage_scan_gba(const char *path, uint32_t offset,
                     storage_entry_t *entries, size_t capacity,
                     uint32_t *total, size_t *count)
{
    if (total) *total = 0;
    if (count) *count = 0;
    if (!path || !path[0] || !total || !count || (!entries && capacity))
        return -EINVAL;
    Request r = {.operation = FS_SCAN_GBA, .path = path, .buffer = entries,
                 .size = capacity, .scan_offset = offset,
                 .total = total, .count = count};
    return call(&r);
}
int storage_crc32(const char *path, uint32_t *crc, uint32_t *bytes)
{
    if (!path || !crc || !bytes) return -EINVAL;
    Request r = {.operation = FS_CRC, .path = path, .crc = crc, .bytes = bytes};
    return call(&r);
}
int storage_write_test(void) { Request r = {.operation = FS_TEST}; return call(&r); }
int storage_tf_probe(void) { Request r = {.operation = FS_TF_PROBE}; return call(&r); }
uint32_t storage_media_sectors(void) { return sdspi_sector_count(); }
int regba_fs_open(const char *path, int flags, int mode)
{
    if (!path) return -EINVAL;
    Request r = {.operation = FS_OPEN, .path = path, .flags = flags, .mode = mode};
    return call(&r);
}
int regba_fs_close(int fd) { Request r = {.operation = FS_CLOSE, .fd = fd}; return call(&r); }
int regba_fs_sync(int fd) { Request r = {.operation = FS_SYNC, .fd = fd}; return call(&r); }
int regba_fs_read(int fd, void *buffer, size_t size)
{
    if (!buffer && size) return -EINVAL;
    Request r = {.operation = FS_READ, .fd = fd, .buffer = buffer, .size = size};
    return call(&r);
}
int regba_fs_write(int fd, const void *buffer, size_t size)
{
    if (!buffer && size) return -EINVAL;
    Request r = {.operation = FS_WRITE, .fd = fd, .buffer = (void *)buffer, .size = size};
    return call(&r);
}
long regba_fs_lseek(int fd, long offset, int whence)
{
    Request r = {.operation = FS_SEEK, .fd = fd, .offset = offset, .whence = whence};
    return call(&r);
}
int regba_fs_fstat(int fd, struct stat *st)
{
    if (!st) return -EINVAL;
    Request r = {.operation = FS_FSTAT, .fd = fd, .buffer = st}; return call(&r);
}
int regba_fs_stat(const char *path, struct stat *st)
{
    if (!path || !st) return -EINVAL;
    Request r = {.operation = FS_STAT, .path = path, .buffer = st}; return call(&r);
}
int regba_fs_unlink(const char *path)
{
    if (!path) return -EINVAL;
    Request r = {.operation = FS_UNLINK, .path = path}; return call(&r);
}
int regba_fs_rename(const char *old_path, const char *new_path)
{
    if (!old_path || !new_path) return -EINVAL;
    Request r = {.operation = FS_RENAME, .path = old_path, .new_path = new_path};
    return call(&r);
}
#else
/* The existing synthetic FatFs host model calls the exact production scan. */
int storage_scan_gba(const char *path, uint32_t offset,
                     storage_entry_t *entries, size_t capacity,
                     uint32_t *total, size_t *count)
{
    return scan_gba_directory(path, offset, entries, capacity, total, count);
}
#endif

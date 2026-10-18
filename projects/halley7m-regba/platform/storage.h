#ifndef REGBA_STORAGE_H
#define REGBA_STORAGE_H

#include <stddef.h>
#include <stdint.h>
#include <sys/stat.h>

/* All calls are synchronous RPCs to one filesystem owner task. Call only from
 * task context after vTaskStartScheduler(). Results are nonnegative on success,
 * or negative errno values; errno is never shared between the caller and owner.
 * File descriptors 0..2 belong to the runtime UART implementation. Paths are
 * UTF-8, at most 512 bytes; the owner converts to FatFs UTF-16 without truncation. */
int storage_start(void);
int storage_mount(void);             /* No directory creation or test writes. */
int storage_prepare_dirs(void);      /* Explicitly creates /GBA resource dirs. */
int storage_list(const char *path);
typedef struct {
    char name[256];                  /* UTF-8 name, at most 255 bytes. */
    uint32_t size;
    uint8_t directory;
} storage_entry_t;

/* Directory order, counting only visible subdirectories and .gba files.
 * Scans to the end for total, retaining at most capacity entries from offset.
 * capacity=0 permits entries=NULL. Outputs are zero on error. Invalid Unicode
 * or a name exceeding 255 UTF-8 bytes returns an error instead of truncating. */
int storage_scan_gba(const char *path, uint32_t offset,
                     storage_entry_t *entries, size_t capacity,
                     uint32_t *total, size_t *count);
int storage_crc32(const char *path, uint32_t *crc, uint32_t *bytes);
int storage_write_test(void);        /* Explicit /GBA/TEST file-only test. */
int storage_tf_probe(void);          /* GPIO command probe; refuses mounted/open media. */
uint32_t storage_media_sectors(void);

int regba_fs_open(const char *path, int flags, int mode);
int regba_fs_close(int fd);
int regba_fs_read(int fd, void *buffer, size_t size);
int regba_fs_write(int fd, const void *buffer, size_t size);
long regba_fs_lseek(int fd, long offset, int whence);
int regba_fs_fstat(int fd, struct stat *st);
int regba_fs_stat(const char *path, struct stat *st);
int regba_fs_sync(int fd);
int regba_fs_unlink(const char *path);
int regba_fs_rename(const char *old_path, const char *new_path);

#endif

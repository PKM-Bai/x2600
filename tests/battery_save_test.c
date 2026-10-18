/* Compile this file alone. It includes the production writer/loader and uses
 * real local files, with faults at the filesystem operation boundaries. */
#define _CRT_SECURE_NO_WARNINGS
#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#ifndef S_ISREG
#define S_ISREG(mode) (((mode) & S_IFMT) == S_IFREG)
#endif

static FILE *fault_fopen(const char *, const char *);
static size_t fault_fwrite(const void *, size_t, size_t, FILE *);
static size_t fault_fread(void *, size_t, size_t, FILE *);
static int fault_fflush(FILE *);
static int fault_fclose(FILE *);
static int fault_rename(const char *, const char *);
static int fault_remove(const char *);
#define BATTERY_SAVE_HOST_TEST
#define fopen fault_fopen
#define fwrite fault_fwrite
#define fread fault_fread
#define fflush fault_fflush
#define fclose fault_fclose
#define rename fault_rename
#define remove fault_remove
#include "../projects/halley7m-regba/platform/battery_save.c"
#undef fopen
#undef fwrite
#undef fread
#undef fflush
#undef fclose
#undef rename
#undef remove

enum fault_kind { NONE, OPEN, WRITE, READ, FLUSH, CLOSE, ROTATE, COMMIT,
                  COMMIT_ROLLBACK, REMOVE_OLD_BACKUP };
static enum fault_kind fault;
static unsigned rename_calls;
static uint8_t original[SAVE_MAX_BYTES], replacement[SAVE_MAX_BYTES];
static uint8_t loaded[SAVE_MAX_BYTES];
static const char *save_path = "battery-test.sav";

static FILE *fault_fopen(const char *path, const char *mode)
{
    if (fault == OPEN) { fault = NONE; errno = EIO; return NULL; }
    return fopen(path, mode);
}
static size_t fault_fwrite(const void *data, size_t size, size_t count, FILE *file)
{
    if (fault == WRITE) {
        size_t done;
        fault = NONE;
        done = fwrite(data, size, count / 2u, file);
        errno = ENOSPC;
        return done;
    }
    return fwrite(data, size, count, file);
}
static size_t fault_fread(void *data, size_t size, size_t count, FILE *file)
{
    if (fault == READ) {
        size_t done;
        fault = NONE;
        done = fread(data, size, count / 2u, file);
        errno = EIO;
        return done;
    }
    return fread(data, size, count, file);
}
static int fault_fflush(FILE *file)
{
    int result = fflush(file);
    if (fault == FLUSH) { fault = NONE; errno = EIO; return EOF; }
    return result;
}
static int fault_fclose(FILE *file)
{
    int result = fclose(file);
    if (fault == CLOSE) { fault = NONE; errno = EIO; return EOF; }
    return result;
}
static int fault_rename(const char *from, const char *to)
{
    ++rename_calls;
    if ((fault == ROTATE && rename_calls == 1u) ||
        (fault == COMMIT && rename_calls == 2u) ||
        (fault == COMMIT_ROLLBACK && rename_calls >= 2u)) {
        errno = EIO;
        return -1;
    }
    return rename(from, to);
}
static int fault_remove(const char *path)
{
    if (fault == REMOVE_OLD_BACKUP && strstr(path, ".bak")) {
        errno = EACCES;
        return -1;
    }
    return remove(path);
}

static void clean(void)
{
    fault = NONE;
    rename_calls = 0;
    (void)remove(save_path);
    (void)remove("battery-test.sav.bak");
    (void)remove("battery-test.sav.tmp");
}
static void create_file(const char *path, const void *data, size_t size)
{
    FILE *file = fopen(path, "wb");
    assert(file);
    assert(fwrite(data, 1u, size, file) == size);
    assert(fclose(file) == 0);
}
static void check_file(const char *path, const void *data, size_t size)
{
    FILE *file = fopen(path, "rb");
    assert(file);
    assert(fread(loaded, 1u, size, file) == size);
    assert(memcmp(loaded, data, size) == 0);
    assert(fgetc(file) == EOF);
    assert(fclose(file) == 0);
}

int main(void)
{
    const size_t sizes[] = {512u, 8192u, 32768u, 65536u, SAVE_MAX_BYTES};
    const enum fault_kind faults[] = {OPEN, WRITE, FLUSH, CLOSE, ROTATE,
                                      COMMIT, REMOVE_OLD_BACKUP};
    size_t i, size;
    for (i = 0; i < sizeof(original); ++i) {
        original[i] = (uint8_t)((i * 37u) ^ (i >> 8u));
        replacement[i] = (uint8_t)(original[i] ^ 0xa5u);
    }
    for (i = 0; i < sizeof(sizes) / sizeof(sizes[0]); ++i) {
        size_t capacity = sizes[i];
        clean();
        assert(battery_save_load(save_path, loaded, sizeof(loaded), &size) == -ENOENT);
        assert(size == 0);
        assert(battery_save_test_write(save_path, original, capacity) == 0);
        assert(battery_save_load(save_path, loaded, sizeof(loaded), &size) == 0);
        assert(size == capacity && memcmp(loaded, original, size) == 0);
        assert(battery_save_test_write(save_path, replacement, capacity) == 0);
        check_file(save_path, replacement, capacity);
        check_file("battery-test.sav.bak", original, capacity);
    }
    for (i = 0; i < sizeof(faults) / sizeof(faults[0]); ++i) {
        clean();
        create_file(save_path, original, SAVE_MAX_BYTES);
        create_file("battery-test.sav.bak", original, SAVE_MAX_BYTES);
        fault = faults[i];
        assert(battery_save_test_write(save_path, replacement, SAVE_MAX_BYTES) < 0);
        fault = NONE;
        check_file(save_path, original, SAVE_MAX_BYTES);
    }
    clean();
    create_file(save_path, original, SAVE_MAX_BYTES);
    fault = COMMIT_ROLLBACK;
    assert(battery_save_test_write(save_path, replacement, SAVE_MAX_BYTES) < 0);
    fault = NONE;
    assert(battery_save_load(save_path, loaded, sizeof(loaded), &size) == 0);
    assert(size == SAVE_MAX_BYTES && memcmp(loaded, original, size) == 0);

    clean();
    create_file("battery-test.sav.bak", original, 512u);
    create_file("battery-test.sav.tmp", replacement, 8192u);
    assert(battery_save_load(save_path, loaded, sizeof(loaded), &size) == 0);
    assert(size == 512u && memcmp(loaded, original, size) == 0);
    /* A completed .sav wins even if old .bak and stray .tmp exist. */
    create_file("battery-test.sav.bak", replacement, 512u);
    assert(battery_save_load(save_path, loaded, sizeof(loaded), &size) == 0);
    assert(size == 512u && memcmp(loaded, original, size) == 0);

    clean();
    create_file(save_path, original, 513u);
    assert(battery_save_load(save_path, loaded, sizeof(loaded), &size) == -EILSEQ);
    assert(size == 0);
    create_file(save_path, original, 8192u);
    assert(battery_save_load(save_path, loaded, 512u, &size) == -EILSEQ);
    fault = READ;
    assert(battery_save_load(save_path, loaded, sizeof(loaded), &size) == -EIO);
    assert(size == 0);
    fault = CLOSE;
    assert(battery_save_load(save_path, loaded, sizeof(loaded), &size) == -EIO);
    assert(size == 0);
    assert(battery_save_test_write(save_path, replacement, 513u) == -EINVAL);
    clean();
    puts("Battery save checks passed: five sizes, replacement/previous version, "
         "short write/read, flush/close, rename/rollback and recovery errors.");
    return 0;
}

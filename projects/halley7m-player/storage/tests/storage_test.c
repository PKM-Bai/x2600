#include "player_storage.h"
#include "FreeRTOS.h"
#include "ff.h"
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>

static int scheduler, in_isr;
static uintptr_t task = 1;
static uint32_t file_bytes = UINT32_MAX;
static unsigned open_calls, seek_calls, read_calls, closed_directories;
static FRESULT next_read_result = FR_OK;
static uint16_t last_path[512];

int xPortIsInsideInterrupt(void) { return in_isr; }
int xTaskGetSchedulerState(void) { return scheduler; }
TaskHandle_t xTaskGetCurrentTaskHandle(void) { return (void *)task; }
FRESULT f_mount(FATFS *fs, const TCHAR *path, uint8_t opt)
{ assert(path[0] == '0' && opt == 1); fs->mounted = 1; return FR_OK; }
FRESULT f_open(FIL *fp, const TCHAR *path, uint8_t mode)
{
    assert(mode == FA_READ);
    ++open_calls;
    size_t length = 0;
    do { last_path[length] = path[length]; } while (path[length++]);
    fp->obj.objsize = file_bytes;
    fp->fptr = 0;
    return FR_OK;
}
FRESULT f_close(FIL *fp) { (void)fp; return FR_OK; }
FRESULT f_read(FIL *fp, void *buffer, UINT bytes, UINT *received)
{
    ++read_calls;
    if (next_read_result != FR_OK) return next_read_result;
    assert(bytes <= fp->obj.objsize - fp->fptr);
    if (bytes) memset(buffer, 0x5a, bytes);
    fp->fptr += bytes;
    *received = bytes;
    return FR_OK;
}
FRESULT f_lseek(FIL *fp, FSIZE_t offset)
{ ++seek_calls; assert(offset <= fp->obj.objsize); fp->fptr = offset; return FR_OK; }
FRESULT f_opendir(DIR *directory, const TCHAR *path)
{ assert(path[0] != 0); directory->index = 0; return FR_OK; }
FRESULT f_readdir(DIR *directory, FILINFO *entry)
{
    memset(entry, 0, sizeof(*entry));
    if (directory->index++ == 0) {
        static const TCHAR name[] = {0x4e2d, '.', 'w', 'a', 'v', 0};
        memcpy(entry->fname, name, sizeof(name));
        entry->fsize = 4321;
    }
    return FR_OK;
}
FRESULT f_closedir(DIR *directory) { (void)directory; ++closed_directories; return FR_OK; }

static int visit(void *opaque, const char *name, uint64_t bytes, int directory)
{
    unsigned *count = opaque;
    ++*count;
    assert(!strcmp(name, "\xe4\xb8\xad.wav"));
    assert(bytes == 4321 && !directory);
    return 17;
}

int main(void)
{
    assert(player_storage_mount() == -EAGAIN);
    scheduler = taskSCHEDULER_RUNNING;
    assert(player_storage_mount() == 0);
    struct player_storage_file *file = NULL, *other = NULL;
    assert(player_storage_open_utf8(&file, "0:/\xe4\xb8\xad.avi") == 0);
    assert(last_path[3] == 0x4e2d);
    const struct player_media_io *io = player_storage_io(file);
    assert(io && io->size(io->opaque) == UINT32_MAX);
    assert(player_storage_open_utf8(&other, "0:/other.avi") == -EBUSY && !other);
    unsigned before = seek_calls;
    assert(io->seek(io->opaque, UINT64_C(0x100000000)) == -EOVERFLOW);
    assert(io->seek(io->opaque, UINT64_MAX) == -EOVERFLOW);
    assert(seek_calls == before);
    assert(io->seek(io->opaque, UINT32_MAX - 1u) == 0);
    unsigned char bytes[16] = {0};
    assert(io->read(io->opaque, bytes, sizeof(bytes)) == 1 && bytes[0] == 0x5a);
    assert(io->read(io->opaque, bytes, sizeof(bytes)) == 0);
    before = read_calls;
    assert(io->read(io->opaque, bytes, UINT32_MAX) == -EOVERFLOW);
    assert(read_calls == before);
    task = 2;
    assert(player_storage_mount() == -EPERM);
    assert(io->read(io->opaque, bytes, 1) == -EPERM);
    assert(player_storage_close(file) == -EPERM);
    task = 1;
    in_isr = 1;
    assert(io->read(io->opaque, bytes, 1) == -EAGAIN);
    in_isr = 0;
    assert(io->seek(io->opaque, 0) == 0);
    next_read_result = FR_DISK_ERR;
    assert(io->read(io->opaque, bytes, 1) == -EIO);
    next_read_result = FR_OK;
    unsigned entries = 0;
    assert(player_storage_ls_utf8("0:/MEDIA", visit, &entries) == 17);
    assert(entries == 1 && closed_directories == 1);
    assert(player_storage_close(file) == 0);
    assert(player_storage_io(file) == NULL);
    assert(io->read(io->opaque, bytes, 1) == -EBADF);
    before = open_calls;
    assert(player_storage_open_utf8(&file, "0:/\xc0\xaf") == -EILSEQ);
    assert(!file && open_calls == before);
    file_bytes = 16;
    assert(player_storage_open_utf8(&file, "0:/small.wav") == 0);
    io = player_storage_io(file);
    assert(io->seek(io->opaque, 17) == -EOVERFLOW);
    assert(player_storage_close(file) == 0);
    printf("PASS storage readonly modes/UTF-8/4GiB seek/EOF/task ownership/IO errors/list cleanup\n");
    return 0;
}

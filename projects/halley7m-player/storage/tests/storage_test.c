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
static unsigned open_calls, seek_calls, read_calls, closed_directories, closed_files;
static FRESULT next_read_result = FR_OK;
static FRESULT mount_result = FR_NOT_READY;
static unsigned gpio_probes;
static unsigned map_calls, pump_calls;
static uint16_t last_path[512];
static uint64_t now_us;
uint64_t platform_time_us(void) { return now_us++; }
void vTaskDelay(TickType_t ticks) { now_us += (uint64_t)ticks * 1000u; }

int xPortIsInsideInterrupt(void) { return in_isr; }
int xTaskGetSchedulerState(void) { return scheduler; }
TaskHandle_t xTaskGetCurrentTaskHandle(void) { return (void *)task; }
FRESULT f_mount(FATFS *fs, const TCHAR *path, uint8_t opt)
{ assert(path[0] == '0' && opt == 1); fs->mounted = mount_result == FR_OK; return mount_result; }
FRESULT f_open(FIL *fp, const TCHAR *path, uint8_t mode)
{
    assert(mode == FA_READ);
    ++open_calls;
    size_t length = 0;
    do { last_path[length] = path[length]; } while (path[length++]);
    fp->obj.objsize = file_bytes;
    fp->fptr = 0;
    fp->cltbl = NULL;
    return FR_OK;
}
FRESULT f_close(FIL *fp) { (void)fp; ++closed_files; return FR_OK; }
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
{
    ++seek_calls;
    if (offset == CREATE_LINKMAP) {
        assert(fp->cltbl && fp->cltbl[0] >= 4u);
        ++map_calls; fp->cltbl[0] = 4; fp->cltbl[1] = 1; fp->cltbl[2] = 2; fp->cltbl[3] = 0;
        return FR_OK;
    }
    assert(offset <= fp->obj.objsize); fp->fptr = offset; return FR_OK;
}
static int seek_pump(void *opaque) { assert(opaque == &pump_calls); ++pump_calls; return 0; }
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
int sdspi_gpio_probe(void) { ++gpio_probes; return 0; }

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
    static const uint8_t embedded[] = {0x11, 0x22, 0x33, 0x44, 0x55};
    struct player_storage_file *file = NULL, *other = NULL;
    assert(player_storage_mount() == -EAGAIN);
    assert(player_storage_gpio_probe() == -EAGAIN && !gpio_probes);
    assert(player_storage_register_embedded(embedded, sizeof(embedded)) == -EAGAIN);
    scheduler = taskSCHEDULER_RUNNING;
    assert(player_storage_mount() == -ENODEV);
    assert(player_storage_gpio_probe() == 0 && gpio_probes == 1u);
    assert(player_storage_open_utf8(&file, "@embedded") == -ENOENT && !file);
    assert(player_storage_register_embedded(embedded, sizeof(embedded)) == 0);
    assert(player_storage_open_utf8(&file, "@embedded") == 0);
    assert(player_storage_prepare_seek(file, NULL, NULL) == 0);
    player_storage_seek_stats seek_stats;
    assert(player_storage_get_seek_stats(file, &seek_stats) == 0 && !seek_stats.enabled && !seek_stats.capacity_entries);
    assert(player_storage_gpio_probe() == -EBUSY && gpio_probes == 1u);
    assert(player_storage_register_embedded(embedded, sizeof(embedded)) == -EBUSY);
    assert(player_storage_open_utf8(&other, "@embedded") == -EBUSY && !other);
    const struct player_media_io *memory_io = player_storage_io(file);
    uint8_t memory_bytes[8] = {0};
    assert(memory_io && memory_io->size(memory_io->opaque) == sizeof(embedded));
    assert(memory_io->read(memory_io->opaque, memory_bytes, 3) == 3 && memory_bytes[2] == 0x33);
    assert(memory_io->seek(memory_io->opaque, 4) == 0);
    assert(memory_io->read(memory_io->opaque, memory_bytes, sizeof(memory_bytes)) == 1 && memory_bytes[0] == 0x55);
    assert(memory_io->read(memory_io->opaque, memory_bytes, 1) == 0);
    assert(memory_io->seek(memory_io->opaque, 6) == -EOVERFLOW);
    task = 2;
    assert(player_storage_prepare_seek(file, NULL, NULL) == -EPERM);
    assert(player_storage_get_seek_stats(file, &seek_stats) == -EPERM);
    assert(player_storage_gpio_probe() == -EPERM && gpio_probes == 1u);
    assert(memory_io->read(memory_io->opaque, memory_bytes, 1) == -EPERM);
    assert(player_storage_register_embedded(embedded, sizeof(embedded)) == -EPERM);
    assert(player_storage_close(file) == -EPERM);
    task = 1;
    assert(player_storage_close(file) == 0);
    assert(!open_calls && !read_calls && !seek_calls && !closed_files);
    assert(memory_io->read(memory_io->opaque, memory_bytes, 1) == -EBADF);
    mount_result = FR_OK;
    assert(player_storage_mount() == 0);
    assert(player_storage_gpio_probe() == -EBUSY && gpio_probes == 1u);
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
    struct player_storage_directory *directory = NULL, *other_directory = NULL;
    player_storage_entry entry;
    assert(player_storage_dir_open(&directory, "0:/MEDIA") == 0);
    assert(player_storage_dir_open(&other_directory, "0:/") == -EBUSY && !other_directory);
    task = 2;
    assert(player_storage_dir_next(directory, &entry) == -EPERM);
    assert(player_storage_dir_close(directory) == -EPERM);
    task = 1;
    assert(player_storage_dir_next(directory, &entry) == 0);
    assert(!strcmp(entry.name, "\xe4\xb8\xad.wav") && entry.bytes == 4321 && !entry.is_directory);
    assert(player_storage_dir_next(directory, &entry) == 1);
    assert(player_storage_dir_close(directory) == 0 && closed_directories == 2);
    assert(player_storage_dir_next(directory, &entry) == -EBADF);
    assert(player_storage_dir_close(directory) == -EBADF);
    assert(player_storage_close(file) == 0);
    assert(player_storage_io(file) == NULL);
    assert(io->read(io->opaque, bytes, 1) == -EBADF);
    before = open_calls;
    assert(player_storage_open_utf8(&file, "0:/\xc0\xaf") == -EILSEQ);
    assert(!file && open_calls == before);
    file_bytes = 16;
    assert(player_storage_open_utf8(&file, "0:/small.wav") == 0);
    before = seek_calls;
    assert(player_storage_prepare_seek(file, NULL, NULL) == 0 && seek_calls == before);
    assert(player_storage_get_seek_stats(file, &seek_stats) == 0 && !seek_stats.capacity_entries);
    assert(player_storage_prepare_indexed_seek(file, NULL, NULL) == -EINVAL);
    assert(player_storage_prepare_indexed_seek(file, seek_pump, &pump_calls) == 0);
    assert(map_calls == 1 && pump_calls == 2);
    assert(player_storage_get_seek_stats(file, &seek_stats) == 0 && seek_stats.enabled && seek_stats.used_entries == 4);
    assert(player_storage_prepare_indexed_seek(file, seek_pump, &pump_calls) == 0 && map_calls == 1);
    io = player_storage_io(file);
    assert(io->seek(io->opaque, 17) == -EOVERFLOW);
    assert(player_storage_close(file) == 0);
    printf("PASS storage readonly/UTF-8/4GiB/task ownership/embedded without TF/GPIO probe gates/directory cursor\n");
    return 0;
}

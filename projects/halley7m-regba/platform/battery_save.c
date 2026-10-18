/* One native battery-file snapshot, written outside the emulation task. */
#include "battery_save.h"
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#define SAVE_MAX_BYTES (128u * 1024u)
#define SAVE_PATH_BYTES 513u
#define SAVE_AUX_PATH_BYTES (SAVE_PATH_BYTES + 4u)

static int io_error(void)
{
    return -(errno ? errno : EIO);
}

static int valid_size(size_t size)
{
    return size == 512u || size == 8192u || size == 32768u ||
           size == 65536u || size == SAVE_MAX_BYTES;
}

static int make_aux_path(char *out, const char *path, const char *suffix)
{
    size_t length;
    if (!path || !path[0]) return -EINVAL;
    length = strlen(path);
    if (length >= SAVE_PATH_BYTES) return -ENAMETOOLONG;
    memcpy(out, path, length);
    memcpy(out + length, suffix, 5u); /* Includes the trailing zero. */
    return 0;
}

static int path_exists(const char *path)
{
    struct stat info;
    if (stat(path, &info) == 0)
        return S_ISREG(info.st_mode) ? 1 : -EINVAL;
    return errno == ENOENT ? 0 : io_error();
}

int battery_save_recover(const char *path)
{
    char backup[SAVE_AUX_PATH_BYTES];
    int result = make_aux_path(backup, path, ".bak");
    if (result < 0) return result;
    result = path_exists(path);
    if (result != 0) return result < 0 ? result : 0;
    result = path_exists(backup);
    if (result <= 0) return result < 0 ? result : -ENOENT;
    return rename(backup, path) == 0 ? 0 : io_error();
}

int battery_save_load(const char *path, void *data, size_t capacity, size_t *size)
{
    struct stat info;
    FILE *file;
    int result;
    if (!data || !size) return -EINVAL;
    *size = 0;
    result = battery_save_recover(path);
    if (result < 0) return result;
    if (stat(path, &info) != 0) return io_error();
    if (info.st_size < 0 || !valid_size((size_t)info.st_size) ||
        (size_t)info.st_size > capacity) return -EILSEQ;
    errno = 0;
    file = fopen(path, "rb");
    if (!file) return io_error();
    result = fread(data, 1u, (size_t)info.st_size, file) == (size_t)info.st_size
                 ? 0 : io_error();
    errno = 0;
    if (fclose(file) != 0 && result == 0) result = io_error();
    if (result == 0) *size = (size_t)info.st_size;
    return result;
}

static int write_snapshot(const char *path, const void *data, size_t size)
{
    char temporary[SAVE_AUX_PATH_BYTES], backup[SAVE_AUX_PATH_BYTES];
    const uint8_t *bytes = data;
    FILE *file;
    size_t offset;
    int result, had_save;
    if (!data || !valid_size(size)) return -EINVAL;
    result = make_aux_path(temporary, path, ".tmp");
    if (result < 0) return result;
    result = make_aux_path(backup, path, ".bak");
    if (result < 0) return result;

    /* A previous interrupted rename must be recovered before replacing .bak. */
    result = battery_save_recover(path);
    if (result < 0 && result != -ENOENT) return result;
    errno = 0;
    file = fopen(temporary, "wb");
    if (!file) return io_error();
    result = 0;
    for (offset = 0; offset < size; ) {
        size_t amount = size - offset;
        if (amount > 4096u) amount = 4096u;
        errno = 0;
        if (fwrite(bytes + offset, 1u, amount, file) != amount) {
            result = io_error();
            break;
        }
        offset += amount;
    }
    errno = 0;
    if (fflush(file) != 0 && result == 0) result = io_error();
    errno = 0;
    /* The storage syscall's f_close performs FatFs f_sync. */
    if (fclose(file) != 0 && result == 0) result = io_error();
    if (result < 0) {
        (void)remove(temporary);
        return result;
    }

    had_save = path_exists(path);
    if (had_save < 0) return had_save;
    if (had_save) {
        if (remove(backup) != 0 && errno != ENOENT) return io_error();
        if (rename(path, backup) != 0) return io_error();
    }
    if (rename(temporary, path) != 0) {
        result = io_error();
        if (had_save) (void)rename(backup, path);
        /* Even if rollback fails, .bak remains recoverable on next load. */
        return result;
    }
    return 0;
}

#ifdef BATTERY_SAVE_HOST_TEST
int battery_save_test_write(const char *path, const void *data, size_t size)
{
    return write_snapshot(path, data, size);
}
#else
#include "FreeRTOS.h"
#include "task.h"
#include "platform_runtime.h"
#include "storage.h"
#include "x2600_hal.h"

typedef struct {
    uint32_t queued, completed, errors;
    uint32_t bytes, crc32;
    int busy, last_result;
    char path[SAVE_PATH_BYTES];
} save_state_t;

static save_state_t state;
static uint8_t snapshot[SAVE_MAX_BYTES];
static TaskHandle_t save_task_handle;

static uint32_t snapshot_crc(const uint8_t *data, size_t size)
{
    uint32_t crc = UINT32_MAX;
    while (size--) {
        unsigned bit;
        crc ^= *data++;
        for (bit = 0; bit < 8; ++bit)
            crc = (crc >> 1) ^ ((crc & 1u) ? 0xedb88320u : 0u);
    }
    return crc ^ UINT32_MAX;
}

static void save_task(void *argument)
{
    (void)argument;
    for (;;) {
        int result;
        uint32_t crc;
        (void)ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        crc = snapshot_crc(snapshot, state.bytes);
        result = storage_prepare_dirs();
        if (result == 0) result = write_snapshot(state.path, snapshot, state.bytes);
        taskENTER_CRITICAL();
        state.crc32 = crc;
        state.last_result = result;
        state.completed++;
        if (result < 0) state.errors++;
        taskEXIT_CRITICAL();
        /* Keep ownership until the summary is printed, so a new submission
         * cannot change its path/size while this task reports completion. */
        prom_printk("SAVE complete result=%d bytes=%lu crc32=%08lx path=%s\n",
                    result, (unsigned long)state.bytes, (unsigned long)crc,
                    state.path);
        taskENTER_CRITICAL();
        state.busy = 0;
        taskEXIT_CRITICAL();
    }
}

int battery_save_start(void)
{
    if (save_task_handle) return 0;
    if (xTaskCreate(save_task, "battery_save", 2048, NULL, 2,
                    &save_task_handle) != pdPASS) return -ENOMEM;
    return 0;
}

int battery_save_submit(const char *path, const void *data, size_t size)
{
    size_t length;
    if (!save_task_handle) return -ENXIO;
    if (!path || !data || !valid_size(size)) return -EINVAL;
    length = strlen(path);
    if (!length) return -EINVAL;
    if (length >= SAVE_PATH_BYTES) return -ENAMETOOLONG;
    taskENTER_CRITICAL();
    if (state.busy) {
        taskEXIT_CRITICAL();
        return -EBUSY;
    }
    state.busy = 1;
    taskEXIT_CRITICAL();
    /* The guest is paused at a frame boundary. Copy without disabling IRQs;
     * audio DMA and higher-priority tasks continue during the snapshot. */
    memcpy(snapshot, data, size);
    taskENTER_CRITICAL();
    memcpy(state.path, path, length + 1u);
    state.bytes = (uint32_t)size;
    state.crc32 = 0;
    state.queued++;
    taskEXIT_CRITICAL();
    xTaskNotifyGive(save_task_handle);
    return 0;
}

int battery_save_busy(void)
{
    int busy;
    taskENTER_CRITICAL();
    busy = state.busy;
    taskEXIT_CRITICAL();
    return busy;
}

int battery_save_result(void)
{
    int result;
    taskENTER_CRITICAL();
    result = state.last_result;
    taskEXIT_CRITICAL();
    return result;
}

int battery_save_wait(void)
{
    while (battery_save_busy()) vTaskDelay(1);
    return battery_save_result();
}

void battery_save_status(void)
{
    save_state_t status;
    taskENTER_CRITICAL();
    status = state;
    taskEXIT_CRITICAL();
    prom_printk("SAVE queued=%lu completed=%lu errors=%lu busy=%u bytes=%lu "
                "crc32=%08lx last_result=%d path=%s\n",
                (unsigned long)status.queued, (unsigned long)status.completed,
                (unsigned long)status.errors, (unsigned)status.busy,
                (unsigned long)status.bytes, (unsigned long)status.crc32,
                status.last_result, status.path[0] ? status.path : "-");
}
#endif

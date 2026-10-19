/* Compiles the actual TF benchmark with an in-memory storage/OST boundary. */
#include "player_probe.h"
#include "player_media.h"
#include <assert.h>
#include <errno.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct player_storage_file { int opened; };
static struct player_storage_file fake_file;
static uint64_t fake_size, available, position, now_us;
static uint32_t read_calls, delay_calls, amp_calls;
static int mount_error, open_error, close_error, read_error_at, overread;
static uint32_t close_calls;
static char output[1024];
static int fake_printf(const char *format, ...);
static int player_storage_mount(void);
static int player_storage_open_utf8(struct player_storage_file **file, const char *path);
static const struct player_media_io *player_storage_io(struct player_storage_file *file);
static int player_storage_close(struct player_storage_file *file);
static void player_amp_service(void) { ++amp_calls; }
static uint64_t platform_time_us(void) { return now_us; }
static void vTaskDelay(unsigned ticks) { ++delay_calls; now_us += (uint64_t)ticks * 1000u; }

#define printf fake_printf
/* BENCHMARK_PRODUCTION */
#undef printf

static int fake_printf(const char *format, ...)
{
    va_list arguments;
    va_start(arguments, format);
    if (!strncmp(format, "TF_BENCH result=", 16))
        (void)vsnprintf(output, sizeof(output), format, arguments);
    va_end(arguments);
    return 0;
}

static int32_t fake_read(void *opaque, void *buffer, uint32_t bytes)
{
    (void)opaque;
    assert(buffer && bytes && bytes <= 16384u && fake_file.opened);
    ++read_calls;
    now_us += 100u + read_calls % 3u * 20u;
    if (read_error_at && read_calls == (uint32_t)read_error_at) return -EIO;
    if (overread) return (int32_t)bytes + 1;
    if (bytes > available - position) bytes = (uint32_t)(available - position);
    position += bytes;
    return (int32_t)bytes;
}

static uint64_t fake_size_read(void *opaque) { (void)opaque; return fake_size; }
static const struct player_media_io fake_io = {NULL, fake_read, NULL, fake_size_read};
static int player_storage_mount(void) { return mount_error; }
static int player_storage_open_utf8(struct player_storage_file **file, const char *path)
{
    assert(path && !strcmp(path, "0:/test.avi"));
    if (open_error) return open_error;
    assert(!fake_file.opened);
    fake_file.opened = 1;
    *file = &fake_file;
    return 0;
}
static const struct player_media_io *player_storage_io(struct player_storage_file *file)
{
    assert(file == &fake_file && file->opened);
    return &fake_io;
}
static int player_storage_close(struct player_storage_file *file)
{
    assert(file == &fake_file && file->opened);
    ++close_calls;
    file->opened = 0;
    return close_error;
}

static void reset(uint64_t size)
{
    memset(&fake_file, 0, sizeof(fake_file));
    fake_size = available = size;
    position = now_us = 0;
    read_calls = delay_calls = amp_calls = close_calls = 0;
    mount_error = open_error = close_error = read_error_at = overread = 0;
    output[0] = 0;
}
static uint64_t field(const char *key)
{
    char pattern[64];
    (void)snprintf(pattern, sizeof(pattern), "%s=", key);
    const char *found = strstr(output, pattern);
    assert(found);
    return strtoull(found + strlen(pattern), NULL, 10);
}
static void result(int expected)
{
    assert(strstr(output, "TF_BENCH result=") == output);
    assert(strtol(output + strlen("TF_BENCH result="), NULL, 10) == expected);
    assert(!fake_file.opened);
}

int main(void)
{
    reset(17001);
    player_probe_benchmark("0:/test.avi", 8u * 1024u * 1024u);
    result(0);
    assert(field("bytes") == 17001 && field("target_bytes") == 17001);
    assert(field("eof") == 1 && field("truncated") == 0);
    assert(read_calls == 2 && delay_calls == 2 && close_calls == 1 && amp_calls >= 5);
    assert(field("min_read_us") == 120 && field("max_read_us") == 140);
    assert(field("elapsed_us") == 2260 && field("read_us") == 260);
    assert(field("Bps") == UINT64_C(17001) * 1000000u / 2260u);

    reset(10u * 1024u * 1024u);
    player_probe_benchmark("0:/test.avi", 0);
    result(0);
    assert(field("bytes") == 8u * 1024u * 1024u && field("eof") == 0);

    reset(0);
    player_probe_benchmark("0:/test.avi", 0);
    result(0);
    assert(!read_calls && field("eof") == 1 && !field("min_read_us") && !field("Bps"));

    reset(20000);
    available = 17000;
    player_probe_benchmark("0:/test.avi", 0);
    result(-EIO);
    assert(field("bytes") == 17000 && field("eof") == 1 && field("truncated") == 1);
    assert(close_calls == 1 && read_calls == 3);

    reset(20000);
    read_error_at = 2;
    close_error = -EBADF;
    player_probe_benchmark("0:/test.avi", 0);
    result(-EIO); /* Preserve the first failure while still closing. */
    assert(field("bytes") == 16384 && close_calls == 1);

    reset(10);
    overread = 1;
    player_probe_benchmark("0:/test.avi", 0);
    result(-EIO);
    assert(field("bytes") == 0 && close_calls == 1);

    reset(10);
    close_error = -EBADF;
    player_probe_benchmark("0:/test.avi", 0);
    result(-EBADF);

    reset(UINT32_MAX);
    player_probe_benchmark("0:/test.avi", UINT32_MAX);
    result(0);
    assert(field("bytes") == UINT32_MAX && position == UINT32_MAX);
    assert(field("Bps") == (uint64_t)UINT32_MAX * 1000000u / now_us);
    assert(read_calls == 262144 && field("eof") == 1);

    reset(UINT64_C(0x100000000));
    player_probe_benchmark("0:/test.avi", 0);
    result(-EOVERFLOW);
    assert(!read_calls && close_calls == 1);

    reset(100);
    player_probe_benchmark("@embedded", 0);
    result(-EINVAL);
    assert(!read_calls && !close_calls);

    reset(100);
    open_error = -ENOENT;
    player_probe_benchmark("0:/test.avi", 0);
    result(-ENOENT);
    assert(!read_calls && !close_calls);

    puts("PASS TF benchmark production: short file/empty/limit/EOF, truncation, read/close errors, overread, <4GiB totals/Bps, embedded rejection; simulated IO does not measure TF speed");
    return 0;
}

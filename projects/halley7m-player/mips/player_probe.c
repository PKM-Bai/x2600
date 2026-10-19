#include "player_probe.h"
#include "player_platform.h"
#include "player_storage.h"
#include "player_media.h"
#include "player_codec.h"
#include "player_display.h"
#include "FreeRTOS.h"
#include "task.h"
#include <errno.h>
#include <malloc.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Keep allocations reachable if either DMA engine cannot be stopped. */
static player_video_pool_t pool;
static player_codec_t *codec;
static player_codec_buffer_t buffers[PLAYER_VIDEO_SLOTS];
static uint8_t *packet_buffer;
static bool pool_initialized;
static bool codec_running;
static uint32_t presented;
static uint64_t first_latch_us, second_latch_us, last_latch_us, pending_due_us, max_late_us;
static uint64_t first_ready_us;
static uint32_t late_frames;
static bool verify_frames;
static uint32_t verified_frames;

#if PLAYER_HAVE_TEST_MEDIA
extern const uint8_t player_test_media_start[], player_test_media_end[];
static uint64_t test_position;
static uint64_t test_size(void *unused)
{
    (void)unused;
    return (uint64_t)(player_test_media_end - player_test_media_start);
}
static int test_seek(void *unused, uint64_t absolute)
{
    if (absolute > test_size(unused)) return -1;
    test_position = absolute;
    return 0;
}
static int32_t test_read(void *unused, void *buffer, uint32_t bytes)
{
    uint64_t available = test_size(unused) - test_position;
    if (bytes > available) bytes = (uint32_t)available;
    memcpy(buffer, player_test_media_start + (uint32_t)test_position, bytes);
    test_position += bytes;
    return (int32_t)bytes;
}
#endif

static int list_entry(void *unused, const char *name, uint64_t size, int directory)
{
    (void)unused;
    player_amp_service();
    printf("%c %10llu %s\n", directory ? 'D' : 'F', (unsigned long long)size, name);
    return 0;
}

void player_probe_list(const char *path)
{
    int result = player_storage_mount();
    if (!result) result = player_storage_ls_utf8(*path ? path : "0:/", list_entry, NULL);
    printf("PLAYER_LS result=%d\n", result);
}

/* TF_BENCHMARK_BEGIN: host regression compiles these production functions. */
#define PLAYER_BENCHMARK_DEFAULT_BYTES (8u * 1024u * 1024u)
_Alignas(64) static uint8_t benchmark_buffer[16u * 1024u];

struct player_benchmark_stats {
    uint64_t file_bytes, target_bytes, bytes;
    uint64_t elapsed_us, read_us, min_read_us, max_read_us;
    uint32_t reads, eof, truncated;
    int result;
};

static void benchmark_read(const struct player_media_io *io,
                           struct player_benchmark_stats *stats)
{
    uint64_t started = platform_time_us();
    stats->min_read_us = UINT64_MAX;
    while (stats->bytes < stats->target_bytes) {
        uint64_t remaining = stats->target_bytes - stats->bytes;
        uint32_t request = remaining < sizeof(benchmark_buffer) ?
                           (uint32_t)remaining : (uint32_t)sizeof(benchmark_buffer);
        player_amp_service();
        uint64_t before = platform_time_us();
        int32_t received = io->read(io->opaque, benchmark_buffer, request);
        uint64_t duration = platform_time_us() - before;
        stats->read_us += duration;
        if (duration < stats->min_read_us) stats->min_read_us = duration;
        if (duration > stats->max_read_us) stats->max_read_us = duration;
        ++stats->reads;
        player_amp_service();
        if (received < 0) { stats->result = received; break; }
        if ((uint32_t)received > request) { stats->result = -EIO; break; }
        if (!received) {
            stats->eof = stats->truncated = 1;
            stats->result = -EIO; /* Premature EOF, not a successful short file. */
            break;
        }
        stats->bytes += (uint32_t)received;
        vTaskDelay(1);
    }
    stats->elapsed_us = platform_time_us() - started;
    if (!stats->reads) stats->min_read_us = 0;
    if (!stats->result && stats->bytes == stats->file_bytes) stats->eof = 1;
}

void player_probe_benchmark(const char *path, uint32_t maxbytes)
{
    struct player_storage_file *file = NULL;
    struct player_benchmark_stats stats = {0};
    uint32_t requested = maxbytes ? maxbytes : PLAYER_BENCHMARK_DEFAULT_BYTES;
    if (!path || !*path || !strcmp(path, "@embedded")) {
        stats.result = -EINVAL;
        goto done;
    }
    player_amp_service();
    stats.result = player_storage_mount();
    if (!stats.result) stats.result = player_storage_open_utf8(&file, path);
    if (stats.result) goto done;
    const struct player_media_io *io = player_storage_io(file);
    if (!io || !io->read || !io->size) { stats.result = -EBADF; goto done; }
    stats.file_bytes = io->size(io->opaque);
    if (stats.file_bytes >= UINT64_C(0x100000000)) {
        stats.result = -EOVERFLOW;
        goto done;
    }
    stats.target_bytes = stats.file_bytes < requested ? stats.file_bytes : requested;
    printf("TF_BENCH_BEGIN requested=%lu file_bytes=%llu target_bytes=%llu chunk_bytes=%lu path=%s\n",
           (unsigned long)requested, (unsigned long long)stats.file_bytes,
           (unsigned long long)stats.target_bytes, (unsigned long)sizeof(benchmark_buffer), path);
    benchmark_read(io, &stats);
done:
    if (file) {
        int closed = player_storage_close(file);
        if (!stats.result) stats.result = closed;
    }
    /* File/request are below 4GiB, so bytes*1e6 fits a uint64_t. */
    uint64_t wall_bps = stats.elapsed_us ? stats.bytes * UINT64_C(1000000) / stats.elapsed_us : 0;
    uint64_t read_bps = stats.read_us ? stats.bytes * UINT64_C(1000000) / stats.read_us : 0;
    printf("TF_BENCH result=%d bytes=%llu target_bytes=%llu file_bytes=%llu elapsed_us=%llu Bps=%llu read_us=%llu read_Bps=%llu reads=%lu min_read_us=%llu max_read_us=%llu eof=%lu truncated=%lu yield_ticks=1 source=tf-readonly\n",
           stats.result, (unsigned long long)stats.bytes, (unsigned long long)stats.target_bytes,
           (unsigned long long)stats.file_bytes, (unsigned long long)stats.elapsed_us,
           (unsigned long long)wall_bps, (unsigned long long)stats.read_us,
           (unsigned long long)read_bps, (unsigned long)stats.reads,
           (unsigned long long)stats.min_read_us, (unsigned long long)stats.max_read_us,
           (unsigned long)stats.eof, (unsigned long)stats.truncated);
}
/* TF_BENCHMARK_END */

static int open_media(const char *path, struct player_storage_file **file,
                       struct player_media *media)
{
    int result;
#if PLAYER_HAVE_TEST_MEDIA
    if (!strcmp(path, "@embedded")) {
        struct player_media_io io = { NULL, test_read, test_seek, test_size };
        test_position = 0;
        result = player_media_open(media, &io);
    } else
#endif
    {
        result = player_storage_mount();
        if (!result) result = player_storage_open_utf8(file, path);
        if (!result) result = player_media_open(media, player_storage_io(*file));
    }
    if (!result)
        printf("MEDIA_HEADER kind=%u bytes=%llu frames=%llu samples=%llu size=%lux%lu fps=%lu/%lu\n",
               (unsigned)media->kind, (unsigned long long)media->file_size,
               (unsigned long long)media->video_frames, (unsigned long long)media->audio_samples,
               (unsigned long)media->width, (unsigned long)media->height,
               (unsigned long)media->fps_num, (unsigned long)media->fps_den);
    else printf("MEDIA_OPEN result=%d path=%s\n", result, path);
    return result;
}

static bool interrupted(void)
{
    player_amp_service();
    int c = platform_uart_getchar();
    return c == 'q' || c == 'Q' || c == 3;
}

void player_probe_media(const char *path)
{
    struct player_storage_file *file = NULL;
    struct player_media media;
    struct player_media_packet packet;
    uint64_t video = 0, samples = 0, bytes = 0;
    uint32_t maximum = 0;
    int result = open_media(path, &file, &media);
    uint8_t *data = NULL;
    if (result) goto done;
    data = memalign(128, PLAYER_MEDIA_VIDEO_MAX + PLAYER_MEDIA_INPUT_PADDING);
    if (!data) { result = -ENOMEM; goto done; }
    printf("MEDIA_SCAN cancel=q audio_output=disabled\n");
    while (!(result = player_media_next(&media, &packet))) {
        if (interrupted()) { result = -ECANCELED; break; }
        result = player_media_read_packet(&media, &packet, data,
                                           PLAYER_MEDIA_VIDEO_MAX + PLAYER_MEDIA_INPUT_PADDING);
        if (result) break;
        bytes += packet.length;
        if (packet.kind == PLAYER_PACKET_VIDEO) {
            ++video;
            if (packet.length > maximum) maximum = packet.length;
        } else samples += packet.length / media.audio_block_align;
        vTaskDelay(1);
    }
done:
    free(data);
    if (file) (void)player_storage_close(file);
    printf("MEDIA_SCAN result=%d video=%llu samples=%llu bytes=%llu max_video=%lu\n",
           result, (unsigned long long)video, (unsigned long long)samples,
           (unsigned long long)bytes, (unsigned long)maximum);
}

static int stop_video(void)
{
    int decoder_result = codec_running ? player_codec_stop(codec, pool.session) : 0;
    if (!decoder_result) codec_running = false;
    int display_result = player_display_stop();
    if (decoder_result || display_result) {
        printf("VIDEO_PINNED codec_stop=%d display_stop=%d; retry: video stop\n",
               decoder_result, display_result);
        return decoder_result ? decoder_result : display_result;
    }
    if (codec) player_codec_close(codec);
    codec = NULL;
    if (pool_initialized) player_video_pool_stop(&pool);
    for (unsigned i = 0; i < PLAYER_VIDEO_SLOTS; ++i) {
        free(buffers[i].base);
        buffers[i].base = NULL;
    }
    free(packet_buffer);
    packet_buffer = NULL;
    return 0;
}

void player_probe_stop(void) { (void)stop_video(); }

static int poll_display(bool wait)
{
    uint64_t deadline = platform_time_us() + 250000u;
    for (;;) {
        player_display_completion_t completion;
        int result = player_display_poll_completion(&completion);
        if (result < 0) return result;
        if (result) {
            if (!player_video_display_latch(&pool, completion.latched)) return -ESTALE;
            if (!presented) first_latch_us = completion.irq_time_us;
            if (presented == 1) second_latch_us = completion.irq_time_us;
            last_latch_us = completion.irq_time_us;
            uint64_t late = pending_due_us && last_latch_us > pending_due_us ?
                            last_latch_us - pending_due_us : 0;
            if (late > max_late_us) max_late_us = late;
            if (late > 33334u) ++late_frames;
            ++presented;
            if (presented <= 3 || !(presented % 30u))
                printf("VIDEO_LATCH n=%lu slot=%lu irq=%lu desc=%08lx y=%08lx site=%08lx\n",
                       (unsigned long)presented, (unsigned long)completion.latched.slot,
                       (unsigned long)completion.irq_sequence, (unsigned long)completion.descriptor,
                       (unsigned long)completion.physical_y, (unsigned long)completion.read_site);
            return 0;
        }
        if (!wait) return 0;
        if (interrupted()) return -ECANCELED;
        if (platform_time_us() >= deadline) return -ETIMEDOUT;
        vTaskDelay(1);
    }
}

static int present_ready(uint64_t *clock_start)
{
    player_codec_frame_t frame;
    int result;
    while (!(result = player_codec_get_ready(codec, pool.session, &frame))) {
        if (verify_frames || !presented) {
            /* Read the DMA result through KSEG1. Do not clean stale CPU lines
             * over FELIX output; checksum only visible pixels, not padding. */
            uint32_t crc[2] = { UINT32_MAX, UINT32_MAX };
            const uint8_t *planes[2] = {
                (const uint8_t *)(uintptr_t)(frame.physical_y | 0xa0000000u),
                (const uint8_t *)(uintptr_t)(frame.physical_uv | 0xa0000000u)
            };
            for (unsigned p = 0; p < 2; ++p)
                for (uint32_t row = 0; row < frame.height / (p ? 2u : 1u); ++row)
                    for (uint32_t x = 0; x < frame.width; ++x) {
                        crc[p] ^= planes[p][row * frame.stride + x];
                        for (unsigned bit = 0; bit < 8; ++bit)
                            crc[p] = (crc[p] >> 1) ^ (0xedb88320u & (0u - (crc[p] & 1u)));
                    }
            printf("%s index=%llu y_crc32=%08lx uv_crc32=%08lx\n",
                   verify_frames ? "VIDEO_FRAME_CRC" : "VIDEO_FIRST_FRAME",
                   (unsigned long long)frame.frame_index,
                   (unsigned long)~crc[0], (unsigned long)~crc[1]);
        }
        if (verify_frames) {
            ++verified_frames;
            result = player_codec_return_frame(codec, frame.token);
            if (result) return result;
            continue;
        }
        uint64_t due = *clock_start ?
            *clock_start + frame.frame_index * UINT64_C(1000000) / 30u : 0;
        while (due && platform_time_us() < due) {
            if (interrupted()) return -ECANCELED;
            vTaskDelay(1);
        }
        if (!player_video_display_submit(&pool, frame.token)) return -EINVAL;
        pending_due_us = due;
        player_display_frame_t display = {
            .nv12 = frame.y, .capacity = buffers[frame.token.slot].capacity, .token = frame.token
        };
        result = player_display_submit_frame(&display);
        if (result) {
            (void)player_video_display_release(&pool, frame.token);
            return result;
        }
        result = poll_display(true);
        if (result) return result;
        /* First-frame scanout and the panel's 100ms backlight settling belong
         * to LOADING. Start the media timeline once that setup has returned. */
        if (!*clock_start) {
            *clock_start = platform_time_us();
            first_ready_us = *clock_start;
        }
    }
    return result == -EAGAIN ? 0 : result;
}

static void run_video(const char *path, unsigned long frame_limit, bool keep_last, bool verify)
{
    struct player_storage_file *file = NULL;
    struct player_media media;
    struct player_media_packet packet;
    player_video_layout_t layout;
    uint64_t clock_start = 0, decode_us = 0;
    uint32_t decoded = 0;
    size_t minimum_heap = platform_free_heap();
    presented = 0;
    first_latch_us = second_latch_us = last_latch_us = max_late_us = first_ready_us = 0;
    late_frames = 0;
    if (codec || packet_buffer) {
        printf("VIDEO_BUSY retry: video stop\n");
        return;
    }
    verify_frames = verify;
    verified_frames = 0;
    if (!pool_initialized) { player_video_pool_init(&pool); pool_initialized = true; }
    int result = open_media(path, &file, &media);
    if (result) goto done;
    if (media.kind != PLAYER_MEDIA_AVI) { result = -ENOTSUP; goto done; }
    result = player_video_layout_nv12(720, 1280, &layout);
    if (result) goto done;
    packet_buffer = memalign(128, PLAYER_MEDIA_VIDEO_MAX + PLAYER_MEDIA_INPUT_PADDING);
    if (!packet_buffer) { result = -ENOMEM; goto done; }
    for (unsigned i = 0; i < PLAYER_VIDEO_SLOTS; ++i) {
        buffers[i].capacity = layout.total_bytes;
        buffers[i].base = memalign(128, layout.total_bytes);
        if (!buffers[i].base) { result = -ENOMEM; goto done; }
        memset(buffers[i].base, 0, layout.total_bytes);
    }
    result = player_codec_open(&codec, &pool, pool.session, buffers);
    if (result) goto done;
    codec_running = true;
    if (!verify_frames) {
        result = player_display_init();
        if (!result) result = player_display_start();
    }
    if (result) goto done;
    if (platform_free_heap() < minimum_heap) minimum_heap = platform_free_heap();
    printf("VIDEO_PROBE session=%lu stride=768 bytes_per_slot=%lu limit=%lu cancel=q audio_output=disabled clock=diagnostic-wall\n",
           (unsigned long)pool.session, (unsigned long)layout.total_bytes, frame_limit);
    while (!(result = player_media_next(&media, &packet))) {
        if (interrupted()) { result = -ECANCELED; break; }
        if (packet.kind != PLAYER_PACKET_VIDEO) continue;
        result = player_media_read_packet(&media, &packet, packet_buffer,
                                           PLAYER_MEDIA_VIDEO_MAX + PLAYER_MEDIA_INPUT_PADDING);
        if (result) break;
        uint64_t start = platform_time_us();
        result = player_codec_submit(codec, pool.session, packet_buffer, packet.length,
                                       PLAYER_MEDIA_VIDEO_MAX + PLAYER_MEDIA_INPUT_PADDING, packet.index);
        decode_us += platform_time_us() - start;
        if (platform_free_heap() < minimum_heap) minimum_heap = platform_free_heap();
        if (result) break;
        ++decoded;
        result = present_ready(&clock_start);
        if (result || (frame_limit && decoded >= frame_limit)) break;
    }
    if (!result || result == PLAYER_MEDIA_EOF) {
        result = player_codec_drain(codec, pool.session);
        if (result >= 0) result = present_ready(&clock_start);
    }
done:
    if (file) (void)player_storage_close(file);
    player_display_stats_t stats = { 0 };
    if (!verify_frames) player_display_get_stats(&stats);
    int held = keep_last && !result && presented;
    int stopped = held ? 0 : stop_video();
    uint64_t span = last_latch_us - first_latch_us;
    uint32_t fps_milli = span ? (uint32_t)((uint64_t)(presented - 1u) * 1000000000u / span) : 0;
    uint64_t steady_span = last_latch_us - second_latch_us;
    uint32_t steady_fps = presented > 2 && steady_span ?
        (uint32_t)((uint64_t)(presented - 2u) * 1000000000u / steady_span) : 0;
    printf("VIDEO_PROBE result=%d stop=%d decoded=%lu presented=%lu decode_us=%llu elapsed_us=%llu underruns=%lu mismatches=%lu heap=%lu sampled_min_free_heap=%lu latch_fps_milli=%lu steady_fps_milli=%lu startup_us=%llu late_frames=%lu max_late_us=%llu held=%d verified=%lu\n",
           result, stopped, (unsigned long)decoded, (unsigned long)presented,
           (unsigned long long)decode_us,
           (unsigned long long)(clock_start ? platform_time_us() - clock_start : 0),
           (unsigned long)stats.underruns, (unsigned long)stats.mismatches,
           (unsigned long)platform_free_heap(), (unsigned long)minimum_heap,
           (unsigned long)fps_milli, (unsigned long)steady_fps,
           (unsigned long long)(first_ready_us ? first_ready_us - first_latch_us : 0),
           (unsigned long)late_frames,
           (unsigned long long)max_late_us, held, (unsigned long)verified_frames);
}

void player_probe_video(const char *path, unsigned long frame_limit)
{ run_video(path, frame_limit, false, false); }

void player_probe_video_hold(const char *path)
{ run_video(path, 0, true, false); }

void player_probe_video_verify(const char *path)
{ run_video(path, 0, false, true); }

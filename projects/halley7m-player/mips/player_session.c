#include "player_session.h"
#include "player_session_core.h"
#include "player_config.h"
#include "player_platform.h"
#include "player_storage.h"
#include "player_codec.h"
#include "player_display.h"
#include "player_audio_client.h"
#include "FreeRTOS.h"
#include "task.h"
#include <errno.h>
#include <malloc.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef PLAYER_SESSION_HOST_TEST
#include "player_session_test_alloc.h"
#define memalign player_test_memalign
#define free player_test_free
#endif

#define INITIAL_PCM_SAMPLES 4800u
#define DISPLAY_TIMEOUT_US UINT64_C(250000)

static player_session_snapshot_t view, published;
static player_session_command_t commands[PLAYER_SESSION_COMMANDS];
static unsigned command_read, command_count;
static volatile unsigned cancel_requested;
static unsigned initialized;
static struct player_storage_file *file;
static struct player_storage_directory *directory;
static struct player_media media;
static struct player_media_packet pending_packet;
static unsigned packet_pending, file_eof, codec_drained, audio_eof;
static unsigned audio_started, first_video_latched, video_pending, has_ready;
static uint64_t pending_at;
static uint32_t pending_frame_index;
static unsigned ready_wait_traced, ready_busy_traced, codec_busy_traced;
static uint32_t queued_pcm_samples;
static int pump_error;
static char failure_detail[96];
static player_read_ahead read_ahead;
static uint8_t *read_bytes, *packet_bytes;
static player_packet_pool packet_pool;
static player_queued_packet video_packets[PLAYER_VIDEO_QUEUE_PACKETS];
static player_queued_packet audio_packets[PLAYER_AUDIO_QUEUE_PACKETS];
static player_packet_queue video_queue, audio_queue;
static player_video_pool_t video_pool;
static player_codec_t *codec;
static player_codec_buffer_t frame_buffers[PLAYER_VIDEO_SLOTS];
static player_codec_frame_t ready_frame;
static unsigned codec_running, pool_initialized, video_opened;
static player_audio_client_stats audio_status;
static uint32_t directory_index;
static unsigned activate_when_found;
static player_session_trace_t timing_trace[PLAYER_SESSION_TRACE_CAPACITY];
static uint32_t trace_sequence;
static unsigned trace_state = UINT32_MAX;
#if defined(PLAYER_HAVE_TEST_MEDIA) && PLAYER_HAVE_TEST_MEDIA
extern const uint8_t player_test_media_start[], player_test_media_end[];
#endif

void player_session_trace_reset(void)
{
    taskENTER_CRITICAL();
    trace_sequence = 0;
    trace_state = UINT32_MAX;
    taskEXIT_CRITICAL();
}

unsigned player_session_trace_read(uint32_t after, player_session_trace_t *records,
                                    unsigned capacity, uint32_t *latest)
{
    unsigned copied = 0;
    if (!records && capacity) return 0;
    taskENTER_CRITICAL();
    uint32_t last = trace_sequence;
    uint32_t first = last > PLAYER_SESSION_TRACE_CAPACITY ? last - PLAYER_SESSION_TRACE_CAPACITY + 1u : 1u;
    if (after > last) after = 0;
    if (after + 1u > first) first = after + 1u;
    while (copied < capacity && first <= last) {
        records[copied++] = timing_trace[(first - 1u) % PLAYER_SESSION_TRACE_CAPACITY];
        ++first;
    }
    if (latest) *latest = last;
    taskEXIT_CRITICAL();
    return copied;
}

static void trace_event(enum player_session_trace_event event, uint64_t frame,
                         uint32_t duration, uint32_t irq, int result, uint64_t time)
{
    player_session_trace_t record;
    memset(&record, 0, sizeof(record));
    record.session = view.session_id;
    record.event = (uint32_t)event;
    record.frame = (uint32_t)frame;
    record.time_us_low = (uint32_t)(time ? time : platform_time_us());
    record.audio_samples = audio_status.completed_samples;
    record.duration_us = duration;
    record.irq_sequence = irq;
    record.queues = video_queue.count | (audio_queue.count << 16);
    record.flags = (video_pending ? 1u : 0u) | (has_ready ? 2u : 0u) |
                   (audio_started ? 4u : 0u) | ((uint32_t)view.state << 8);
    for (unsigned i = 0; i < PLAYER_VIDEO_SLOTS; ++i)
        record.holds |= video_pool.slots[i].holds << (i * 4u);
    record.result = result;
    taskENTER_CRITICAL();
    record.sequence = ++trace_sequence;
    timing_trace[(record.sequence - 1u) % PLAYER_SESSION_TRACE_CAPACITY] = record;
    taskEXIT_CRITICAL();
}

static void sync_video_active(void)
{
    player_display_set_video_active(codec && view.state == PLAYER_SESSION_PLAYING);
}

static void publish(void)
{
    sync_video_active();
    if (trace_state != (unsigned)view.state) {
        trace_state = (unsigned)view.state;
        trace_event(PLAYER_SESSION_TRACE_STATE, 0, 0, 0, view.error_code, 0);
    }
    view.video_queue_packets = video_queue.count;
    view.audio_queue_packets = audio_queue.count;
    view.queued_pcm_samples = queued_pcm_samples;
    ++view.revision;
    taskENTER_CRITICAL();
    published = view;
    taskEXIT_CRITICAL();
}

int player_session_post(const player_session_command_t *command)
{
    if (!command || command->kind < PLAYER_SESSION_PLAY_PATH ||
        command->kind > PLAYER_SESSION_LIBRARY_UP)
        return -EINVAL;
    if ((command->kind == PLAYER_SESSION_PLAY_PATH || command->kind == PLAYER_SESSION_LIBRARY_OPEN) &&
        !memchr(command->path, 0, sizeof(command->path)))
        return -ENAMETOOLONG;
    taskENTER_CRITICAL();
    if (command_count == PLAYER_SESSION_COMMANDS) {
        taskEXIT_CRITICAL();
        return -EAGAIN;
    }
    commands[(command_read + command_count) % PLAYER_SESSION_COMMANDS] = *command;
    ++command_count;
    if (command->kind == PLAYER_SESSION_STOP)
        cancel_requested = 1;
    taskEXIT_CRITICAL();
    return 0;
}

int player_session_snapshot(player_session_snapshot_t *snapshot)
{
    if (!snapshot) return -EINVAL;
    taskENTER_CRITICAL();
    *snapshot = published;
    taskEXIT_CRITICAL();
    return 0;
}

static int pop_command(player_session_command_t *command)
{
    taskENTER_CRITICAL();
    if (!command_count) {
        taskEXIT_CRITICAL();
        return 0;
    }
    *command = commands[command_read];
    command_read = (command_read + 1u) % PLAYER_SESSION_COMMANDS;
    --command_count;
    taskEXIT_CRITICAL();
    return 1;
}

static int control_pump(void *unused)
{
    int result = 0;
    (void)unused;
    player_amp_service();
    if (audio_started) {
        result = player_audio_client_service();
        if (result) pump_error = result;
    }
    if (cancel_requested) return -ECANCELED;
    return result;
}

static int poll_video(void)
{
    player_display_completion_t completion;
    int result;
    if (!video_opened) return 0;
    while ((result = player_display_poll_completion(&completion)) > 0) {
#if PLAYER_DISPLAY_HAS_OVERLAY
        if (completion.has_latched) {
#else
        {
#endif
            if (!player_video_display_latch(&video_pool, completion.latched)) return -ESTALE;
            uint64_t delay = completion.irq_time_us >= pending_at ? completion.irq_time_us - pending_at : 0;
            trace_event(PLAYER_SESSION_TRACE_LATCH, pending_frame_index,
                         delay > UINT32_MAX ? UINT32_MAX : (uint32_t)delay,
                         completion.irq_sequence, 0, completion.irq_time_us);
            video_pending = 0;
            first_video_latched = 1;
            ++view.displayed_frames;
        }
#if PLAYER_DISPLAY_HAS_OVERLAY
        else if (completion.has_retired) {
            if (!player_video_display_release(&video_pool, completion.retired)) return -ESTALE;
        }
#endif
    }
    if (result < 0) return result;
    if (video_pending && platform_time_us() - pending_at > DISPLAY_TIMEOUT_US)
        return -ETIMEDOUT;
    return 0;
}

static int detach_video(void)
{
    if (!video_opened) return 0;
#if PLAYER_DISPLAY_HAS_OVERLAY
    uint64_t deadline = platform_time_us() + DISPLAY_TIMEOUT_US;
    int requested = 0;
    while (platform_time_us() < deadline) {
        player_display_stats_t display;
        int result = poll_video();
        if (result) return result;
        if (!requested) {
            result = player_display_clear_video();
            if (!result) requested = 1;
            else if (result != -EAGAIN) return result;
        }
        player_display_get_stats(&display);
        if (requested && !display.scanout) {
            /* EOD may race the stats read. Consume its retirement event before
             * invalidating pool tokens or opening another media session.
             * A subsequent GUI-only pending update does not reference NV12. */
            result = poll_video();
            if (result) return result;
            unsigned held = 0;
            for (unsigned i = 0; i < PLAYER_VIDEO_SLOTS; ++i)
                held |= video_pool.slots[i].holds & (PLAYER_VIDEO_PENDING | PLAYER_VIDEO_SCANOUT);
            if (!held) {
                video_opened = 0;
                video_pending = 0;
                return 0;
            }
        }
        player_amp_service();
        vTaskDelay(1);
    }
    return -ETIMEDOUT;
#else
    int result = player_display_stop();
    if (!result) { video_opened = 0; video_pending = 0; }
    return result;
#endif
}

static int stop_resources(void)
{
    player_display_set_video_active(false);
    /* Audio STOP ACK releases shared CPM ownership before either video
     * engine changes clocks. On failure every allocation remains reachable. */
    int result = player_audio_client_stop();
    int codec_result, display_result;
    if (result) { view.resources_pinned = 1; return result; }
    audio_started = 0;
    codec_result = codec_running ? player_codec_stop(codec, video_pool.session) : 0;
    if (!codec_result) codec_running = 0;
    display_result = detach_video();
    if (codec_result || display_result) {
        view.resources_pinned = 1;
        return codec_result ? codec_result : display_result;
    }
    if (codec) player_codec_close(codec);
    codec = NULL;
    if (pool_initialized) player_video_pool_stop(&video_pool);
    for (unsigned i = 0; i < PLAYER_VIDEO_SLOTS; ++i) {
        free(frame_buffers[i].base);
        frame_buffers[i].base = NULL;
        frame_buffers[i].capacity = 0;
    }
    free(read_bytes); read_bytes = NULL;
    free(packet_bytes); packet_bytes = NULL;
    if (file) {
        result = player_storage_close(file);
        file = NULL;
    }
    memset(&audio_status, 0, sizeof(audio_status));
    packet_pending = file_eof = codec_drained = audio_eof = has_ready = 0;
    queued_pcm_samples = 0;
    video_queue.count = audio_queue.count = 0;
    view.resources_pinned = 0;
    return result;
}

static void fail_session(int error, const char *stage)
{
    int stopped = view.resources_pinned ? error : stop_resources();
    view.state = PLAYER_SESSION_ERROR;
    view.error_code = error ? error : -EIO;
    if (stopped)
        (void)snprintf(view.error, sizeof(view.error), "%s: %s (%d); STOP: %d", stage,
                       failure_detail[0] ? failure_detail : "hardware or I/O error", error, stopped);
    else
        (void)snprintf(view.error, sizeof(view.error), "%s: %s (%d)", stage,
                       failure_detail[0] ? failure_detail : "hardware or I/O error", error);
    publish();
}

static int media_failure(int result, const char *operation)
{
    if (read_ahead.error) {
        (void)snprintf(failure_detail, sizeof(failure_detail), "%s: file read error", operation);
        return read_ahead.error;
    }
    (void)snprintf(failure_detail, sizeof(failure_detail), "%s: %s", operation,
                   player_media_result_string(result));
    return pump_error ? pump_error : result;
}

static int begin_media_impl(const char *path)
{
    player_video_layout_t layout;
    int result;
    if (!path || !*path) return -EINVAL;
    if (view.resources_pinned) return -EBUSY;
    failure_detail[0] = 0;
    view.state = PLAYER_SESSION_LOADING;
    publish();
    result = stop_resources();
    if (result) return result;
    view.media_kind = 0;
    view.error_code = 0;
    view.error[0] = 0;
    view.elapsed_samples = view.total_samples = 0;
    view.decoded_frames = view.displayed_frames = view.dropped_frames = view.audio_underruns = 0;
    view.late_frames = view.max_late_us = 0;
    if (++view.session_id == 0) ++view.session_id;
    player_session_trace_reset();
    (void)snprintf(view.media_path, sizeof(view.media_path), "%s", path);
    first_video_latched = 0;
    pump_error = 0;
    publish();
    result = !strcmp(path, "@embedded") ? 0 : player_storage_mount();
    if (!result) result = player_storage_open_utf8(&file, path);
    if (result) return result;
    read_bytes = memalign(128, PLAYER_READ_AHEAD_BYTES);
    packet_bytes = memalign(128, PLAYER_PACKET_POOL_BYTES);
    if (!read_bytes || !packet_bytes) return -ENOMEM;
    result = player_read_ahead_init(&read_ahead, player_storage_io(file), read_bytes,
                                    PLAYER_READ_AHEAD_BYTES, control_pump, NULL);
    if (result) return result;
    result = player_packet_pool_init(&packet_pool, packet_bytes, PLAYER_PACKET_POOL_BYTES);
    if (result) return result;
    result = player_media_open(&media, &read_ahead.io);
    if (result) return media_failure(result, "header");
    if (!media.audio_samples || media.audio_samples > UINT32_MAX / 2u ||
        (media.kind == PLAYER_MEDIA_AVI && media.audio_samples != media.video_frames * 1600u))
    {
        (void)snprintf(failure_detail, sizeof(failure_detail), "empty/long PCM or unequal AVI audio/video duration");
        return -ENOTSUP;
    }
    view.media_kind = (uint32_t)media.kind;
    view.total_samples = (uint32_t)media.audio_samples;
    player_packet_queue_init(&video_queue, video_packets, PLAYER_VIDEO_QUEUE_PACKETS);
    player_packet_queue_init(&audio_queue, audio_packets, PLAYER_AUDIO_QUEUE_PACKETS);
    if (media.kind == PLAYER_MEDIA_AVI) {
        result = player_video_layout_nv12(720, 1280, &layout);
        if (result) return result;
        for (unsigned i = 0; i < PLAYER_VIDEO_SLOTS; ++i) {
            if ((result = control_pump(NULL)) != 0) return result;
            frame_buffers[i].capacity = layout.total_bytes;
            frame_buffers[i].base = memalign(128, layout.total_bytes);
            if (!frame_buffers[i].base) return -ENOMEM;
            memset(frame_buffers[i].base, 0, layout.total_bytes);
        }
        result = player_codec_open(&codec, &video_pool, video_pool.session, frame_buffers);
        if (result) return result;
        codec_running = 1;
        result = player_display_start();
        if (result) return result;
        video_opened = 1;
    }
    return 0;
}

static int begin_media(const char *path)
{
    int result = begin_media_impl(path);
    if (result && !(result == -ECANCELED && cancel_requested) &&
        (view.state == PLAYER_SESSION_LOADING || view.resources_pinned))
        fail_session(result, "open media");
    return result;
}

static int demux_one(void)
{
    player_queued_packet queued;
    player_packet_queue *queue;
    uint32_t padding;
    int result;
    if (file_eof) return 0;
    if (!packet_pending) {
        result = player_media_next(&media, &pending_packet);
        if (result == PLAYER_MEDIA_EOF) { file_eof = 1; return 0; }
        if (result) return media_failure(result, "packet header");
        packet_pending = 1;
    }
    queue = pending_packet.kind == PLAYER_PACKET_VIDEO ? &video_queue : &audio_queue;
    if (queue->count == queue->capacity) return 0;
    padding = pending_packet.kind == PLAYER_PACKET_VIDEO ? PLAYER_MEDIA_INPUT_PADDING : 0;
    memset(&queued, 0, sizeof(queued));
    queued.media = pending_packet;
    result = player_packet_pool_reserve(&packet_pool, pending_packet.length, padding, &queued.allocation);
    if (result == -EAGAIN) return 0;
    if (result) return result;
    result = player_media_read_packet(&media, &pending_packet, queued.allocation.data, queued.allocation.capacity);
    if (result) {
        (void)player_packet_pool_release(&packet_pool, queued.allocation.ticket);
        return media_failure(result, "packet payload");
    }
    result = player_packet_queue_push(queue, &queued);
    if (result) {
        (void)player_packet_pool_release(&packet_pool, queued.allocation.ticket);
        return result;
    }
    if (pending_packet.kind == PLAYER_PACKET_AUDIO)
        queued_pcm_samples += pending_packet.length / 2u;
    packet_pending = 0;
    return 0;
}

static int feed_audio(void)
{
    player_queued_packet *packet;
    int result;
    while ((packet = player_packet_queue_front(&audio_queue)) != NULL) {
        unsigned remaining = packet->media.length / 2u - packet->consumed_samples;
        unsigned copied = player_audio_client_write_pcm((const int16_t *)packet->allocation.data +
                                                        packet->consumed_samples, remaining);
        if (copied > remaining) return -EPROTO;
        packet->consumed_samples += copied;
        queued_pcm_samples -= copied;
        if (packet->consumed_samples != packet->media.length / 2u) break;
        result = player_packet_pool_release(&packet_pool, packet->allocation.ticket);
        if (result) return result;
        player_packet_queue_pop(&audio_queue);
    }
    result = player_audio_client_service();
    if (result) return result;
    if (file_eof && !audio_queue.count && !audio_eof) {
        result = player_audio_client_mark_eof();
        if (result) return result;
        audio_eof = 1;
    }
    player_audio_client_get_status(&audio_status);
    view.elapsed_samples = audio_status.completed_samples;
    view.audio_underruns = audio_status.underruns;
    return audio_status.last_error;
}

static int decode_one(void)
{
    player_queued_packet *packet;
    int result;
    /* A pending display frame owns a distinct pool slot. Decode its successor
     * into a free slot now, rather than adding VPU time after every EOD. */
    if (!codec || has_ready) return 0;
    result = player_codec_get_ready(codec, video_pool.session, &ready_frame);
    if (!result) { has_ready = 1; ready_wait_traced = ready_busy_traced = 0; return 0; }
    if (result != -EAGAIN) return result;
    packet = player_packet_queue_front(&video_queue);
    if (packet) {
        if ((result = control_pump(NULL)) != 0) return result;
        uint64_t decode_at = platform_time_us();
        result = player_codec_submit(codec, video_pool.session, packet->allocation.data,
                                      packet->media.length, packet->allocation.capacity, packet->media.index);
        uint64_t elapsed = platform_time_us() - decode_at;
        if (result == -EAGAIN) {
            if (!codec_busy_traced) {
                trace_event(PLAYER_SESSION_TRACE_CODEC_BUSY, packet->media.index, 0, 0, result, 0);
                codec_busy_traced = 1;
            }
            return 0;
        }
        trace_event(PLAYER_SESSION_TRACE_DECODE, packet->media.index,
                     elapsed > UINT32_MAX ? UINT32_MAX : (uint32_t)elapsed, 0, result, 0);
        codec_busy_traced = 0;
        if (result) return result;
        ++view.decoded_frames;
        result = player_packet_pool_release(&packet_pool, packet->allocation.ticket);
        if (result) return result;
        player_packet_queue_pop(&video_queue);
        if ((result = control_pump(NULL)) != 0) return result;
    } else if (file_eof && !codec_drained) {
        result = player_codec_drain(codec, video_pool.session);
        if (result < 0) return result;
        if (!result) codec_drained = 1;
    }
    result = player_codec_get_ready(codec, video_pool.session, &ready_frame);
    if (!result) { has_ready = 1; ready_wait_traced = ready_busy_traced = 0; }
    return result == -EAGAIN ? 0 : result;
}

static int present_ready(void)
{
    player_display_frame_t frame;
    enum player_video_timing timing;
    int result;
    if (!has_ready || video_pending) return 0;
    if (view.state == PLAYER_SESSION_LOADING) {
        if (first_video_latched || ready_frame.frame_index != 0) return 0;
        timing = PLAYER_VIDEO_PRESENT;
    } else {
        timing = player_video_timing_decide(ready_frame.frame_index, media.video_frames,
                                            audio_status.completed_samples);
    }
    if (timing == PLAYER_VIDEO_WAIT) {
        if (!ready_wait_traced) {
            trace_event(PLAYER_SESSION_TRACE_WAIT_PTS, ready_frame.frame_index, 0, 0, 0, 0);
            ready_wait_traced = 1;
        }
        return 0;
    }
    if (timing == PLAYER_VIDEO_DROP) {
        result = player_codec_return_frame(codec, ready_frame.token);
        if (!result) {
            uint64_t late = (uint64_t)audio_status.completed_samples * 30u - ready_frame.frame_index * 48000u;
            uint64_t microseconds = late * 1000000u / 1440000u;
            if (late >= 48000u) ++view.late_frames;
            if (microseconds > view.max_late_us)
                view.max_late_us = microseconds > UINT32_MAX ? UINT32_MAX : (uint32_t)microseconds;
            has_ready = 0;
            ++view.dropped_frames;
            trace_event(PLAYER_SESSION_TRACE_DROP, ready_frame.frame_index, 0, 0, 0, 0);
        }
        return result;
    }
    frame = (player_display_frame_t){ready_frame.y, frame_buffers[ready_frame.token.slot].capacity,
                                     ready_frame.token};
    /* READY already pins the buffer. Publish PENDING only after the driver
     * accepts it; an overlay update may temporarily return EAGAIN. No other
     * task touches pool metadata, and ISR completion is polled afterward. */
    result = player_display_submit_frame(&frame);
    if (result == -EAGAIN) {
        if (!ready_busy_traced) {
            trace_event(PLAYER_SESSION_TRACE_SUBMIT_BUSY, ready_frame.frame_index, 0, 0, result, 0);
            ready_busy_traced = 1;
        }
        return 0;
    }
    if (result) return result;
    if (!player_video_display_submit(&video_pool, ready_frame.token)) return -EINVAL;
    if (view.state != PLAYER_SESSION_LOADING) {
        uint64_t late = (uint64_t)audio_status.completed_samples * 30u - ready_frame.frame_index * 48000u;
        uint64_t microseconds = late * 1000000u / 1440000u;
        if (late >= 48000u) ++view.late_frames;
        if (microseconds > view.max_late_us)
            view.max_late_us = microseconds > UINT32_MAX ? UINT32_MAX : (uint32_t)microseconds;
    }
    video_pending = 1;
    pending_at = platform_time_us();
    pending_frame_index = (uint32_t)ready_frame.frame_index;
    has_ready = 0;
    trace_event(PLAYER_SESSION_TRACE_SUBMIT, ready_frame.frame_index, 0, 0, 0, pending_at);
    return 0;
}

static int playback_step(void)
{
    int result = poll_video();
    if (result) return result;
    if ((result = control_pump(NULL)) != 0) return result;
    if (audio_started && (result = feed_audio()) != 0) return result;
    if ((result = demux_one()) != 0) return result;
    if (view.state != PLAYER_SESSION_PAUSED) {
        if ((result = decode_one()) != 0) return result;
        if ((result = present_ready()) != 0) return result;
    }
    if (!audio_started && (media.kind == PLAYER_MEDIA_WAV || first_video_latched) &&
        (queued_pcm_samples >= INITIAL_PCM_SAMPLES || file_eof)) {
        /* First NV12 + backlight and at least 100ms PCM are ready before the
         * first DMA can consume five periods. Send the saved PCM as a batch. */
        result = player_audio_client_start(view.total_samples, view.volume_percent);
        audio_started = 1; /* START failure may still own a logical session. */
        if (result) return result;
        if ((result = feed_audio()) != 0) return result;
    }
    if (audio_started) {
        if ((result = feed_audio()) != 0) return result;
        if (view.state == PLAYER_SESSION_LOADING &&
            (audio_status.state == PLAYER_AMP_AUDIO_PLAYING ||
             audio_status.state == PLAYER_AMP_AUDIO_DRAINING ||
             audio_status.state == PLAYER_AMP_AUDIO_DRAINED || audio_status.completed_samples))
            view.state = PLAYER_SESSION_PLAYING;
        if (audio_status.state == PLAYER_AMP_AUDIO_DRAINED && audio_eof &&
            audio_status.completed_samples == view.total_samples && file_eof &&
            (media.kind == PLAYER_MEDIA_WAV || (codec_drained && !video_queue.count &&
                                                !has_ready && !video_pending)))
            view.state = PLAYER_SESSION_ENDED;
    }
    /* Work ahead in small IO units. The parser also uses the same bounded
     * path on a cache miss, pumping heartbeat/audio around every 1KiB read. */
    if (!file_eof) {
        for (unsigned i = 0; i < 8u; ++i) {
            result = player_read_ahead_step(&read_ahead);
            if (result < 0) return result;
            if (!result) break;
        }
    }
    return 0;
}

static int root_path(const char *path)
{
    return !strcmp(path, "0:/") || !strcmp(path, "0:");
}

static void parent_path(char *path)
{
    size_t length = strlen(path);
    while (length > 3u && path[length - 1u] == '/') path[--length] = 0;
    while (length > 3u && path[length - 1u] != '/') path[--length] = 0;
    if (length > 3u) path[length - 1u] = 0;
    else (void)snprintf(path, PLAYER_SESSION_PATH_BYTES, "0:/");
}

static int join_path(char *output, const char *directory_path, const char *name)
{
    size_t parent = strlen(directory_path), child = strlen(name);
    unsigned separator = parent && directory_path[parent - 1u] != '/';
    if (parent + separator + child >= PLAYER_SESSION_PATH_BYTES) return -ENAMETOOLONG;
    memcpy(output, directory_path, parent);
    if (separator) output[parent++] = '/';
    memcpy(output + parent, name, child + 1u);
    return 0;
}

static int media_name(const char *name)
{
    size_t length = strlen(name);
    char extension[4];
    if (length < 4u || name[length - 4u] != '.') return 0;
    for (unsigned i = 0; i < 3u; ++i) {
        char c = name[length - 3u + i];
        extension[i] = c >= 'A' && c <= 'Z' ? (char)(c + ('a' - 'A')) : c;
    }
    extension[3] = 0;
    return !strcmp(extension, "avi") || !strcmp(extension, "wav");
}

static int library_open(const char *path, uint32_t selected, unsigned same_generation)
{
    char saved[PLAYER_SESSION_PATH_BYTES];
    int result;
    (void)snprintf(saved, sizeof(saved), "%s", path && *path ? path : "0:/");
    if (directory) { (void)player_storage_dir_close(directory); directory = NULL; }
    result = player_storage_mount();
    if (!result) result = player_storage_dir_open(&directory, saved);
    view.library_error = result;
    view.library_scanning = result == 0;
    view.library_total_known = result != 0;
    view.library_total = 0;
    view.library_selected = selected;
    view.library_offset = selected / PLAYER_SESSION_LIBRARY_WINDOW * PLAYER_SESSION_LIBRARY_WINDOW;
    view.library_count = 0;
    directory_index = 0;
    if (!same_generation && ++view.library_generation == 0) ++view.library_generation;
    (void)snprintf(view.library_path, sizeof(view.library_path), "%s", saved);
    if (!result && !root_path(saved)) {
        if (!view.library_offset) {
            player_session_entry_t *entry = &view.entries[view.library_count++];
            memset(entry, 0, sizeof(*entry));
            entry->token = view.library_generation;
            entry->is_directory = 1;
            (void)snprintf(entry->label, sizeof(entry->label), "..");
            (void)snprintf(entry->path, sizeof(entry->path), "%s", saved);
            parent_path(entry->path);
        }
        directory_index = view.library_total = 1;
    }
    return result;
}

static int library_activate(uint32_t index)
{
    for (unsigned i = 0; i < view.library_count; ++i) {
        player_session_entry_t *entry = &view.entries[i];
        if (entry->index != index) continue;
        char path[PLAYER_SESSION_PATH_BYTES];
        (void)snprintf(path, sizeof(path), "%s", entry->path);
        activate_when_found = 0;
        return entry->is_directory ? library_open(path, 0, 0) : begin_media(path);
    }
    activate_when_found = 1;
    return 0;
}

static int library_step(void)
{
    player_storage_entry source;
    int result;
    if (!directory) return 0;
    result = player_storage_dir_next(directory, &source);
    if (result) {
        int close_result = player_storage_dir_close(directory);
        directory = NULL;
        view.library_scanning = 0;
        view.library_total_known = 1;
        view.library_total = directory_index;
        view.library_error = result < 0 ? result : close_result;
        if (!view.library_error && view.library_total && view.library_selected >= view.library_total) {
            uint32_t selected = view.library_total - 1u;
            if (selected / PLAYER_SESSION_LIBRARY_WINDOW * PLAYER_SESSION_LIBRARY_WINDOW != view.library_offset)
                return library_open(view.library_path, selected, 1);
            view.library_selected = selected;
        }
        if (!view.library_total) view.library_selected = 0;
        activate_when_found = 0;
        return view.library_error;
    }
    if ((!source.is_directory && !media_name(source.name)) ||
        !strcmp(source.name, ".") || !strcmp(source.name, "..")) return 0;
    if (directory_index >= view.library_offset && view.library_count < PLAYER_SESSION_LIBRARY_WINDOW) {
        player_session_entry_t *entry = &view.entries[view.library_count];
        memset(entry, 0, sizeof(*entry));
        entry->index = directory_index;
        entry->token = view.library_generation;
        entry->is_directory = (uint32_t)source.is_directory;
        entry->bytes = source.bytes;
        (void)snprintf(entry->label, sizeof(entry->label), "%s", source.name);
        result = join_path(entry->path, view.library_path, source.name);
        if (result) { view.library_error = result; return result; }
        ++view.library_count;
    }
    ++directory_index;
    view.library_total = directory_index;
    if (activate_when_found && view.library_selected >= view.library_offset &&
        view.library_selected - view.library_offset < view.library_count)
        return library_activate(view.library_selected);
    return 0;
}

static int handle_command(const player_session_command_t *command)
{
    int result;
    if (command->kind >= PLAYER_SESSION_LIBRARY_MOVE && command->library_generation &&
        command->library_generation != view.library_generation)
        return -ESTALE;
    switch (command->kind) {
    case PLAYER_SESSION_PLAY_PATH:
        return begin_media(command->path);
    case PLAYER_SESSION_STOP:
        result = stop_resources();
        cancel_requested = 0;
        if (!result) {
            view.state = PLAYER_SESSION_IDLE;
            view.error_code = 0;
            view.error[0] = 0;
            view.media_kind = 0;
        }
        return result;
    case PLAYER_SESSION_PAUSE:
    case PLAYER_SESSION_RESUME:
    case PLAYER_SESSION_TOGGLE_PAUSE:
        if (view.state == PLAYER_SESSION_PLAYING && command->kind != PLAYER_SESSION_RESUME) {
            result = player_audio_client_pause();
            if (!result) {
                view.state = PLAYER_SESSION_PAUSED;
                player_audio_client_get_status(&audio_status);
                view.elapsed_samples = audio_status.completed_samples;
            }
            return result;
        }
        if (view.state == PLAYER_SESSION_PAUSED && command->kind != PLAYER_SESSION_PAUSE) {
            result = player_audio_client_resume();
            if (!result) view.state = PLAYER_SESSION_PLAYING;
            return result;
        }
        return -EINVAL;
    case PLAYER_SESSION_SET_VOLUME:
        if (command->value < 0 || command->value > 100) return -EINVAL;
        if (audio_started && (result = player_audio_client_set_volume((unsigned)command->value)) != 0)
            return result;
        view.volume_percent = (uint32_t)command->value;
        return 0;
    case PLAYER_SESSION_LIBRARY_OPEN:
        activate_when_found = 0;
        return library_open(command->path, 0, 0);
    case PLAYER_SESSION_LIBRARY_UP: {
        char path[PLAYER_SESSION_PATH_BYTES];
        (void)snprintf(path, sizeof(path), "%s", view.library_path);
        parent_path(path);
        activate_when_found = 0;
        return library_open(path, 0, 0);
    }
    case PLAYER_SESSION_LIBRARY_MOVE:
    case PLAYER_SESSION_LIBRARY_SELECT:
    case PLAYER_SESSION_LIBRARY_ACTIVATE: {
        if (command->kind == PLAYER_SESSION_LIBRARY_ACTIVATE && command->value < -1)
            return -EINVAL;
        int64_t target = command->kind == PLAYER_SESSION_LIBRARY_MOVE ?
            (int64_t)view.library_selected + command->value :
            (command->kind == PLAYER_SESSION_LIBRARY_ACTIVATE && command->value == -1 ?
                (int64_t)view.library_selected : command->value);
        uint32_t selected;
        if (target < 0) target = 0;
        if (target > UINT32_MAX) target = UINT32_MAX;
        selected = (uint32_t)target;
        if (view.library_total_known && selected >= view.library_total)
            selected = view.library_total ? view.library_total - 1u : 0;
        view.library_selected = selected;
        if (selected / PLAYER_SESSION_LIBRARY_WINDOW * PLAYER_SESSION_LIBRARY_WINDOW != view.library_offset) {
            result = library_open(view.library_path, selected, 1);
            if (result) return result;
        }
        if (command->kind == PLAYER_SESSION_LIBRARY_ACTIVATE)
            return library_activate(selected);
        activate_when_found = 0;
        return 0;
    }
    default: return -EINVAL;
    }
}

int player_session_init(void)
{
    if (initialized) return 0;
    memset(&view, 0, sizeof(view));
    view.state = PLAYER_SESSION_IDLE;
    view.volume_percent = PLAYER_DEFAULT_VOLUME_PERCENT;
    if (!pool_initialized) { player_video_pool_init(&video_pool); pool_initialized = 1; }
#if defined(PLAYER_HAVE_TEST_MEDIA) && PLAYER_HAVE_TEST_MEDIA
    uintptr_t embedded_size = (uintptr_t)player_test_media_end - (uintptr_t)player_test_media_start;
    if (embedded_size && embedded_size <= UINT32_MAX)
        (void)player_storage_register_embedded(player_test_media_start, (uint32_t)embedded_size);
#endif
    initialized = 1;
    (void)library_open("0:/", 0, 0);
    publish();
    return 0;
}

void player_session_step(void)
{
    player_session_command_t command;
    int result;
    if (!initialized) return;
    player_amp_service();
    if (pop_command(&command)) {
        result = handle_command(&command);
        view.last_request_id = command.request_id;
        view.last_request_result = result;
        if (result && result != -EINVAL && result != -EBUSY && result != -ESTALE &&
            view.state != PLAYER_SESSION_ERROR && (command.kind == PLAYER_SESSION_STOP ||
            (audio_started && (command.kind == PLAYER_SESSION_PAUSE || command.kind == PLAYER_SESSION_RESUME ||
             command.kind == PLAYER_SESSION_TOGGLE_PAUSE || command.kind == PLAYER_SESSION_SET_VOLUME))))
            fail_session(result, "command");
    }
    if (view.state == PLAYER_SESSION_LOADING || view.state == PLAYER_SESSION_PLAYING ||
        view.state == PLAYER_SESSION_PAUSED) {
        result = playback_step();
        if (result) {
            if (result == -ECANCELED && cancel_requested) {
                /* STOP is already queued; leave it to own its ACK/result. */
            } else fail_session(result, "playback");
        }
    } else if (audio_started) {
        result = player_audio_client_service();
        if (result && view.state != PLAYER_SESSION_ERROR) fail_session(result, "audio");
    }
    /* Running media gets one directory entry per step. Idle/paused pages may
     * advance sixteen entries while still yielding the AMP service between
     * entries; large libraries need no fixed total allocation or GUI IO. */
    unsigned library_budget = view.state == PLAYER_SESSION_PLAYING || view.state == PLAYER_SESSION_LOADING ? 1u : 16u;
    for (unsigned i = 0; i < library_budget && directory; ++i) {
        (void)library_step();
        player_amp_service();
        if (cancel_requested || view.state == PLAYER_SESSION_LOADING || view.state == PLAYER_SESSION_PLAYING)
            break;
    }
    publish();
}

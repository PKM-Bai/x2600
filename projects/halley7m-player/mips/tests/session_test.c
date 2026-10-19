#include "../player_session.h"
#include "player_storage.h"
#include "player_codec.h"
#include "player_display.h"
#include "player_audio_client.h"
#include "player_config.h"
#include "player_session_test_alloc.h"
#include "task.h"
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint64_t now_us;
static unsigned allocations, audio_starts, stop_fail, codec_stop_fail;
static unsigned request_id;
static uint32_t last_displayed;
static unsigned expected_video_frames = 12u;
static uint8_t wav[44u + 19201u * 2u];
static uint8_t *avi;
static size_t avi_size;
static unsigned mount_failed;
static int seek_prepare_error;
static unsigned cancel_seek_prepare;
static player_session_snapshot_t snapshot;

void *player_test_memalign(size_t alignment, size_t bytes)
{
    void *base = malloc(bytes + alignment + sizeof(void *));
    uintptr_t aligned;
    if (!base) return NULL;
    aligned = ((uintptr_t)base + sizeof(void *) + alignment - 1u) & ~(uintptr_t)(alignment - 1u);
    ((void **)aligned)[-1] = base;
    ++allocations;
    return (void *)aligned;
}
void player_test_free(void *pointer)
{
    if (pointer) { assert(allocations); --allocations; free(((void **)pointer)[-1]); }
}
uint64_t platform_time_us(void) { return now_us; }
void vTaskDelay(TickType_t ticks) { now_us += (uint64_t)ticks * 1000u; }
void player_amp_service(void) { }

struct player_storage_file { const uint8_t *data; uint64_t size, position; unsigned open; };
struct player_storage_directory { unsigned index, open; };
static struct player_storage_file file;
static struct player_storage_directory directory;
static int32_t file_read(void *opaque, void *output, uint32_t bytes)
{
    struct player_storage_file *stream = opaque;
    assert(stream->open && bytes <= 1024u);
    if (bytes > stream->size - stream->position) bytes = (uint32_t)(stream->size - stream->position);
    memcpy(output, stream->data + (size_t)stream->position, bytes);
    stream->position += bytes;
    now_us += 100u;
    return (int32_t)bytes;
}
static int file_seek(void *opaque, uint64_t absolute)
{
    struct player_storage_file *stream = opaque;
    assert(stream->open && absolute <= stream->size);
    stream->position = absolute;
    return 0;
}
static uint64_t file_size(void *opaque) { return ((struct player_storage_file *)opaque)->size; }
int player_storage_mount(void) { return mount_failed ? -ENODEV : 0; }
int player_storage_open_utf8(struct player_storage_file **output, const char *path)
{
    assert(!file.open);
    if (strstr(path, "bad")) return -ENOENT;
    int embedded = !strcmp(path, "@embedded");
    if (mount_failed && !embedded) return -ENODEV;
    file.data = embedded || strstr(path, ".avi") ? avi : wav;
    file.size = embedded || strstr(path, ".avi") ? avi_size : sizeof(wav);
    file.position = 0; file.open = 1; *output = &file;
    return 0;
}
int player_storage_close(struct player_storage_file *stream)
{ assert(stream == &file && file.open); file.open = 0; return 0; }
int player_storage_prepare_seek(struct player_storage_file *stream,
                                player_storage_pump_fn pump, void *opaque)
{
    player_session_snapshot_t current;
    unsigned starts = audio_starts;
    assert(stream == &file && file.open);
    assert(!player_session_snapshot(&current) && current.state == PLAYER_SESSION_LOADING);
    if (cancel_seek_prepare) {
        player_session_command_t stop = {0};
        stop.kind = PLAYER_SESSION_STOP;
        stop.request_id = ++request_id;
        assert(!player_session_post(&stop));
        cancel_seek_prepare = 0;
    }
    int result = pump ? pump(opaque) : 0;
    assert(audio_starts == starts);
    return result ? result : seek_prepare_error;
}
int player_storage_prepare_indexed_seek(struct player_storage_file *stream,
                                        player_storage_pump_fn pump, void *opaque)
{ return player_storage_prepare_seek(stream, pump, opaque); }
int player_storage_get_seek_stats(struct player_storage_file *stream, player_storage_seek_stats *out)
{
    assert((!stream || stream == &file) && out);
    memset(out, 0, sizeof(*out));
    out->capacity_entries = 4096;
    out->required_entries = seek_prepare_error == -E2BIG ? 4098u : 4u;
    return 0;
}
const struct player_media_io *player_storage_io(struct player_storage_file *stream)
{
    static struct player_media_io io;
    io = (struct player_media_io){stream, file_read, file_seek, file_size};
    return &io;
}
int player_storage_dir_open(struct player_storage_directory **output, const char *path)
{ (void)path; assert(!directory.open); directory.open = 1; directory.index = 0; *output = &directory; return 0; }
int player_storage_dir_next(struct player_storage_directory *stream, player_storage_entry *entry)
{
    unsigned index;
    assert(stream == &directory && directory.open);
    index = directory.index++;
    if (index == 27u) return 1;
    memset(entry, 0, sizeof(*entry));
    if (!index) { memcpy(entry->name, "sub", sizeof("sub")); entry->is_directory = 1; }
    else if (index == 26u) memcpy(entry->name, "ignored.txt", sizeof("ignored.txt"));
    else (void)snprintf(entry->name, sizeof(entry->name), "clip%02u.wav", index - 1u);
    return 0;
}
int player_storage_dir_close(struct player_storage_directory *stream)
{ assert(stream == &directory && directory.open); directory.open = 0; return 0; }

static player_audio_client_stats audio;
static uint64_t audio_period_at;
static unsigned audio_eof;
int player_audio_client_service(void)
{
    if (!audio.session_active || audio.state == PLAYER_AMP_AUDIO_PAUSED || audio.state == PLAYER_AMP_AUDIO_DRAINED)
        return 0;
    if (audio.state == PLAYER_AMP_AUDIO_BUFFERING) {
        if (audio.written_samples - audio.completed_samples < 2400u && !audio_eof) return 0;
        audio.state = PLAYER_AMP_AUDIO_PLAYING;
        audio_period_at = now_us;
    }
    while (now_us - audio_period_at >= 10000u) {
        uint32_t available = audio.written_samples - audio.completed_samples;
        uint32_t period = available > 480u ? 480u : available;
        audio_period_at += 10000u;
        if (!period && !audio_eof) { ++audio.underruns; break; }
        audio.completed_samples += period;
        if (audio_eof && audio.completed_samples == audio.total_samples) {
            audio.state = PLAYER_AMP_AUDIO_DRAINED;
            break;
        }
    }
    return 0;
}
int player_audio_client_start(uint32_t total, unsigned volume)
{
    assert(!audio.session_active && volume <= 100u);
    ++audio_starts;
    memset(&audio, 0, sizeof(audio));
    audio.session_active = 1; audio.total_samples = total; audio.volume_percent = volume;
    audio.state = PLAYER_AMP_AUDIO_BUFFERING; audio.clock_valid = 1; audio_eof = 0;
    now_us += 300000u;
    return 0;
}
unsigned player_audio_client_write_pcm(const int16_t *samples, unsigned count)
{
    uint32_t capacity;
    assert(audio.session_active && samples);
    (void)player_audio_client_service();
    capacity = 4800u - (audio.written_samples - audio.completed_samples);
    if (count > capacity) count = capacity;
    audio.written_samples += count;
    return count;
}
int player_audio_client_mark_eof(void)
{ if (!audio.total_samples) audio.total_samples = audio.written_samples;
  assert(audio.written_samples == audio.total_samples); audio_eof = 1; return 0; }
int player_audio_client_pause(void)
{ assert(audio.session_active); (void)player_audio_client_service(); audio.state = PLAYER_AMP_AUDIO_PAUSED; return 0; }
int player_audio_client_resume(void)
{ assert(audio.state == PLAYER_AMP_AUDIO_PAUSED); audio.state = PLAYER_AMP_AUDIO_PLAYING; audio_period_at = now_us; return 0; }
int player_audio_client_set_volume(unsigned volume) { audio.volume_percent = volume; return 0; }
void player_audio_client_get_status(player_audio_client_stats *output)
{ (void)player_audio_client_service(); *output = audio; }
int player_audio_client_stop(void)
{ if (stop_fail && audio.session_active) return -ETIMEDOUT; audio.session_active = 0; return 0; }

struct player_codec {
    player_video_pool_t *pool;
    player_codec_buffer_t buffers[PLAYER_VIDEO_SLOTS];
    player_codec_frame_t ready;
    player_video_token_t reference;
    unsigned running, has_ready, has_reference;
};
static struct player_codec decoder;
int player_codec_open(player_codec_t **output, player_video_pool_t *pool, uint32_t session,
                       const player_codec_buffer_t buffers[PLAYER_VIDEO_SLOTS])
{
    assert(session == pool->session && !decoder.running);
    memset(&decoder, 0, sizeof(decoder));
    decoder.pool = pool;
    memcpy(decoder.buffers, buffers, sizeof(decoder.buffers));
    decoder.running = 1; *output = &decoder; return 0;
}
int player_codec_open_geometry(player_codec_t **output, player_video_pool_t *pool, uint32_t session,
                                const player_codec_buffer_t buffers[PLAYER_VIDEO_SLOTS], uint32_t w, uint32_t h)
{ assert((w == 720 && h == 1280) || (w == 1280 && h == 720));
  return player_codec_open(output, pool, session, buffers); }
int player_codec_submit(player_codec_t *codec, uint32_t session, uint8_t *data,
                         uint32_t length, uint32_t capacity, uint64_t frame)
{
    player_video_token_t token;
    assert(codec->running && session == codec->pool->session && data && length && capacity >= length + 64u);
    if (!player_video_codec_acquire(codec->pool, &token)) return -EAGAIN;
    assert(!codec->has_ready);
    if (codec->has_reference) assert(player_video_codec_release(codec->pool, codec->reference));
    codec->has_reference = 1; codec->reference = token;
    assert(player_video_codec_publish(codec->pool, token));
    memset(&codec->ready, 0, sizeof(codec->ready));
    codec->ready.token = token; codec->ready.frame_index = frame;
    codec->ready.y = codec->buffers[token.slot].base;
    codec->has_ready = 1;
    now_us += 5600u;
    return 0;
}
int player_codec_get_ready(player_codec_t *codec, uint32_t session, player_codec_frame_t *frame)
{ assert(session == codec->pool->session); if (!codec->has_ready) return -EAGAIN; *frame = codec->ready; codec->has_ready = 0; return 0; }
int player_codec_return_frame(player_codec_t *codec, player_video_token_t token)
{ return player_video_discard_ready(codec->pool, token) ? 0 : -EINVAL; }
int player_codec_drain(player_codec_t *codec, uint32_t session)
{ (void)codec; (void)session; return 0; }
int player_codec_stop(player_codec_t *codec, uint32_t session)
{
    assert(!audio.session_active && codec->running && session == codec->pool->session);
    if (codec_stop_fail) return -ETIMEDOUT;
    if (codec->has_reference) assert(player_video_codec_release(codec->pool, codec->reference));
    for (unsigned i = 0; i < PLAYER_VIDEO_SLOTS; ++i) codec->pool->slots[i].holds &= ~PLAYER_VIDEO_READY;
    codec->running = 0; return 0;
}
void player_codec_close(player_codec_t *codec) { assert(!codec->running); }

static player_display_completion_t completion;
static player_video_token_t scanout;
static unsigned display_pending, display_scanout, completion_valid, clear_pending;
static unsigned gui_pending_without_video;
static unsigned video_active;
static uint32_t refresh_us = 1000u;
static uint64_t latch_at;
static unsigned geometry_busy_calls, geometry_in_progress;
int player_display_start(void) { return 0; }
int player_display_set_video_geometry(uint32_t w, uint32_t h)
{
    assert(!display_scanout && ((w == 720 && h == 1280) || (w == 1280 && h == 720)));
    if (geometry_busy_calls) { --geometry_busy_calls; geometry_in_progress = 1; return -EAGAIN; }
    geometry_in_progress = 0; return 0;
}
void player_display_set_video_active(bool active) { video_active = active ? 1u : 0u; }
int player_display_submit_frame(const player_display_frame_t *frame)
{
    if (display_pending || completion_valid) return -EAGAIN;
    memset(&completion, 0, sizeof(completion));
    completion.has_latched = true;
    completion.latched = frame->token;
    completion.has_retired = display_scanout != 0;
    completion.retired = scanout;
    display_pending = 1; latch_at = (now_us / refresh_us + 1u) * refresh_us;
    return 0;
}
int player_display_clear_video(void)
{
    assert(!audio.session_active);
    if (geometry_in_progress) return -EAGAIN;
    if (display_pending || completion_valid) return -EAGAIN;
    if (!display_scanout) return 0;
    memset(&completion, 0, sizeof(completion));
    completion.has_retired = true; completion.retired = scanout;
    clear_pending = display_pending = 1; latch_at = (now_us / refresh_us + 1u) * refresh_us;
    return 0;
}
static void display_advance(void)
{
    if (display_pending && now_us >= latch_at) {
        display_pending = 0; completion_valid = 1;
        display_scanout = !clear_pending;
        if (!clear_pending) scanout = completion.latched;
        clear_pending = 0;
    }
}
int player_display_poll_completion(player_display_completion_t *output)
{
    display_advance();
    if (!completion_valid) return 0;
    *output = completion; completion_valid = 0;
    if (completion.has_latched) last_displayed = (uint32_t)decoder.ready.frame_index;
    return 1;
}
void player_display_get_stats(player_display_stats_t *output)
{ display_advance(); memset(output, 0, sizeof(*output)); output->pending = display_pending != 0 || (!display_scanout && gui_pending_without_video); output->scanout = display_scanout != 0; }

static void put16(uint8_t *p, uint16_t value) { p[0] = (uint8_t)value; p[1] = (uint8_t)(value >> 8); }
static void put32(uint8_t *p, uint32_t value) { put16(p, (uint16_t)value); put16(p + 2, (uint16_t)(value >> 16)); }
static void make_wav(void)
{
    memcpy(wav, "RIFF", 4); put32(wav + 4, sizeof(wav) - 8u); memcpy(wav + 8, "WAVEfmt ", 8);
    put32(wav + 16, 16); put16(wav + 20, 1); put16(wav + 22, 1); put32(wav + 24, 48000);
    put32(wav + 28, 96000); put16(wav + 32, 2); put16(wav + 34, 16); memcpy(wav + 36, "data", 4);
    put32(wav + 40, sizeof(wav) - 44u);
}
static void step(unsigned count)
{
    while (count--) { now_us += 5000u; player_session_step(); assert(player_session_snapshot(&snapshot) == 0); }
}
static void post(enum player_session_command_kind kind, int32_t value, const char *path)
{
    player_session_command_t command;
    memset(&command, 0, sizeof(command)); command.kind = kind; command.value = value;
    command.request_id = ++request_id;
    if (path) (void)snprintf(command.path, sizeof(command.path), "%s", path);
    assert(player_session_post(&command) == 0);
    step(1);
}
static int enqueue(enum player_session_command_kind kind, const char *path)
{
    player_session_command_t command;
    memset(&command, 0, sizeof(command));
    command.kind = kind;
    command.request_id = ++request_id;
    if (path) (void)snprintf(command.path, sizeof(command.path), "%s", path);
    return player_session_post(&command);
}
static void wait_state(enum player_session_state state)
{
    for (unsigned i = 0; i < 5000u && snapshot.state != state; ++i) step(1);
    if (snapshot.state != state) fprintf(stderr, "wanted state %d got %d: %s\n", state, snapshot.state, snapshot.error);
    assert(snapshot.state == state);
}
int main(int argc, char **argv)
{
    assert(argc >= 2 && argc <= 4);
    if (argc >= 3) { expected_video_frames = (unsigned)atoi(argv[2]); refresh_us = 33033u; }
    FILE *input = fopen(argv[1], "rb");
    assert(input && !fseek(input, 0, SEEK_END));
    long length = ftell(input); assert(length > 0);
    avi_size = (size_t)length; avi = malloc(avi_size); assert(avi);
    rewind(input); assert(fread(avi, 1, avi_size, input) == avi_size); fclose(input);
    make_wav();
    assert(player_session_init() == 0);
    if (argc == 4) {
        uint32_t expected_samples = (uint32_t)strtoul(argv[3], NULL, 10);
        for (unsigned run = 0; run < 3; ++run) {
            post(PLAYER_SESSION_PLAY_PATH, 0, "@embedded");
            wait_state(PLAYER_SESSION_PLAYING);
            post(PLAYER_SESSION_PAUSE, 0, NULL);
            uint32_t paused_samples = snapshot.elapsed_samples;
            step(20); assert(snapshot.elapsed_samples == paused_samples);
            post(PLAYER_SESSION_RESUME, 0, NULL);
            wait_state(PLAYER_SESSION_ENDED);
            printf("compressed run=%u kind=%u samples=%u total=%u decoded=%u audio_errors=%u\n",
                   run, snapshot.media_kind, snapshot.elapsed_samples, snapshot.total_samples,
                   snapshot.decoded_frames, snapshot.audio_underruns);
            fflush(stdout);
            assert(snapshot.elapsed_samples == expected_samples && snapshot.total_samples == expected_samples);
            assert(snapshot.decoded_frames == expected_video_frames && !snapshot.audio_underruns);
            post(PLAYER_SESSION_STOP, 0, NULL);
            assert(snapshot.state == PLAYER_SESSION_IDLE && !allocations && !file.open && !audio.session_active);
        }
        free(avi); return 0;
    }
    step(40);
    assert(snapshot.library_total_known && snapshot.library_total == 26u && snapshot.library_count == 10u);
    post(PLAYER_SESSION_LIBRARY_SELECT, 24, NULL);
    step(40);
    assert(snapshot.library_selected == 24u && snapshot.library_offset == 20u && snapshot.library_count == 6u);
    assert(snapshot.entries[4].index == 24u && strstr(snapshot.entries[4].label, "clip23"));
    post(PLAYER_SESSION_LIBRARY_ACTIVATE, 0, NULL);
    step(40);
    assert(!strcmp(snapshot.library_path, "0:/sub") && !strcmp(snapshot.entries[0].label, ".."));
    post(PLAYER_SESSION_LIBRARY_ACTIVATE, 0, NULL);
    assert(!strcmp(snapshot.library_path, "0:/"));
    post(PLAYER_SESSION_LIBRARY_SELECT, 1, NULL);
    player_session_command_t ordered;
    memset(&ordered, 0, sizeof(ordered));
    ordered.kind = PLAYER_SESSION_LIBRARY_MOVE;
    ordered.value = -1;
    ordered.library_generation = snapshot.library_generation;
    ordered.request_id = ++request_id;
    assert(player_session_post(&ordered) == 0);
    ordered.kind = PLAYER_SESSION_LIBRARY_ACTIVATE;
    ordered.request_id = ++request_id;
    assert(player_session_post(&ordered) == 0);
    step(2);
    assert(!strcmp(snapshot.library_path, "0:/sub"));
    post(PLAYER_SESSION_LIBRARY_ACTIVATE, 0, NULL);
    assert(!strcmp(snapshot.library_path, "0:/"));
    for (unsigned i = 0; i < PLAYER_SESSION_COMMANDS; ++i)
        assert(enqueue(PLAYER_SESSION_LIBRARY_MOVE, NULL) == 0);
    assert(enqueue(PLAYER_SESSION_LIBRARY_MOVE, NULL) == -EAGAIN);
    step(PLAYER_SESSION_COMMANDS);
    assert(enqueue(PLAYER_SESSION_PLAY_PATH, "0:/sound.wav") == 0);
    assert(enqueue(PLAYER_SESSION_STOP, NULL) == 0);
    step(1);
    assert(snapshot.state == PLAYER_SESSION_LOADING && !audio_starts);
    step(1);
    assert(snapshot.state == PLAYER_SESSION_IDLE && !allocations && !snapshot.last_request_result);
    seek_prepare_error = -E2BIG;
    post(PLAYER_SESSION_PLAY_PATH, 0, "0:/sound.wav");
    assert(snapshot.state == PLAYER_SESSION_ERROR && snapshot.error_code == -E2BIG &&
           !file.open && !allocations && !audio_starts);
    seek_prepare_error = 0;
    post(PLAYER_SESSION_STOP, 0, NULL);
    cancel_seek_prepare = 1;
    post(PLAYER_SESSION_PLAY_PATH, 0, "0:/sound.wav");
    assert(snapshot.state == PLAYER_SESSION_LOADING && !audio_starts);
    step(1);
    assert(snapshot.state == PLAYER_SESSION_IDLE && !file.open && !allocations && !audio_starts);
    post(PLAYER_SESSION_PLAY_PATH, 0, "0:/sound.wav");
    assert(snapshot.state == PLAYER_SESSION_LOADING && audio_starts == 0);
    wait_state(PLAYER_SESSION_PLAYING);
    assert(audio.written_samples >= 4800u && snapshot.volume_percent == PLAYER_DEFAULT_VOLUME_PERCENT);
    post(PLAYER_SESSION_PAUSE, 0, NULL);
    assert(snapshot.state == PLAYER_SESSION_PAUSED);
    uint32_t paused = snapshot.elapsed_samples;
    step(100);
    assert(snapshot.elapsed_samples == paused);
    post(PLAYER_SESSION_SET_VOLUME, 17, NULL);
    assert(snapshot.volume_percent == 17u);
    post(PLAYER_SESSION_RESUME, 0, NULL);
    wait_state(PLAYER_SESSION_ENDED);
    assert(snapshot.elapsed_samples == 19201u && snapshot.total_samples == 19201u);
    post(PLAYER_SESSION_STOP, 0, NULL);
    assert(snapshot.state == PLAYER_SESSION_IDLE && allocations == 0);
    post(PLAYER_SESSION_PLAY_PATH, 0, "0:/bad.wav");
    assert(snapshot.state == PLAYER_SESSION_ERROR && snapshot.error_code == -ENOENT && allocations == 0);
    step(20); assert(audio_starts == 1u);
    post(PLAYER_SESSION_STOP, 0, NULL);
    post(PLAYER_SESSION_PLAY_PATH, 0, "0:/sound.wav");
    wait_state(PLAYER_SESSION_PLAYING);
    stop_fail = 1;
    unsigned before = allocations;
    post(PLAYER_SESSION_STOP, 0, NULL);
    assert(snapshot.state == PLAYER_SESSION_ERROR && snapshot.resources_pinned && allocations == before);
    post(PLAYER_SESSION_PLAY_PATH, 0, "0:/sound.wav");
    assert(snapshot.last_request_result == -EBUSY && allocations == before);
    stop_fail = 0;
    post(PLAYER_SESSION_STOP, 0, NULL);
    assert(snapshot.state == PLAYER_SESSION_IDLE && !snapshot.resources_pinned && allocations == 0);
    geometry_busy_calls = 3;
    post(PLAYER_SESSION_PLAY_PATH, 0, "0:/picture.avi");
    assert(snapshot.state == PLAYER_SESSION_LOADING && geometry_in_progress);
    post(PLAYER_SESSION_STOP, 0, NULL);
    assert(snapshot.state == PLAYER_SESSION_IDLE && !allocations && !geometry_in_progress);
    geometry_busy_calls = 3;
    post(PLAYER_SESSION_PLAY_PATH, 0, "0:/picture.avi");
    assert(snapshot.state == PLAYER_SESSION_LOADING && geometry_in_progress);
    post(PLAYER_SESSION_PLAY_PATH, 0, "0:/sound.wav");
    wait_state(PLAYER_SESSION_ENDED);
    post(PLAYER_SESSION_STOP, 0, NULL);
    assert(snapshot.state == PLAYER_SESSION_IDLE && !allocations && !geometry_in_progress);
    post(PLAYER_SESSION_PLAY_PATH, 0, "0:/picture.avi");
    wait_state(PLAYER_SESSION_PLAYING);
    now_us += 200000u; /* Late: decode every P frame, skip only presentation. */
    wait_state(PLAYER_SESSION_ENDED);
    assert(snapshot.decoded_frames == expected_video_frames && snapshot.dropped_frames > 0u && last_displayed == expected_video_frames - 1u);
    assert(display_scanout && allocations > 0); /* EOF retains the final image. */
    gui_pending_without_video = 1; /* GUI may keep submitting after NV12 retirement. */
    codec_stop_fail = 1;
    before = allocations;
    post(PLAYER_SESSION_STOP, 0, NULL);
    assert(snapshot.state == PLAYER_SESSION_ERROR && snapshot.resources_pinned && allocations == before);
    codec_stop_fail = 0;
    post(PLAYER_SESSION_STOP, 0, NULL);
    assert(snapshot.state == PLAYER_SESSION_IDLE && allocations == 0 && !display_scanout);
    mount_failed = 1;
    post(PLAYER_SESSION_PLAY_PATH, 0, "@embedded");
    wait_state(PLAYER_SESSION_ENDED);
    assert(snapshot.media_kind == PLAYER_MEDIA_AVI && snapshot.decoded_frames == expected_video_frames &&
           !strcmp(snapshot.media_path, "@embedded"));
    if (argc == 3) {
        printf("session 30Hz timing: decoded=%u displayed=%u dropped=%u late=%u max_late_us=%u\n",
               snapshot.decoded_frames, snapshot.displayed_frames, snapshot.dropped_frames,
               snapshot.late_frames, snapshot.max_late_us);
    }
    assert(!video_active); /* ENDED allows independent GUI transactions. */
    static player_session_trace_t records[PLAYER_SESSION_TRACE_CAPACITY];
    uint32_t latest;
    unsigned count = player_session_trace_read(0, records, PLAYER_SESSION_TRACE_CAPACITY, &latest);
    unsigned overlapped = 0, submitted = 0, latched = 0;
    assert(count && count <= PLAYER_SESSION_TRACE_CAPACITY && latest >= count);
    for (unsigned index = 0; index < count; ++index) {
        assert(records[index].session == snapshot.session_id);
        if (index) assert(records[index].sequence == records[index - 1u].sequence + 1u);
        if (records[index].event == PLAYER_SESSION_TRACE_DECODE && (records[index].flags & 1u)) ++overlapped;
        if (records[index].event == PLAYER_SESSION_TRACE_SUBMIT) ++submitted;
        if (records[index].event == PLAYER_SESSION_TRACE_LATCH) ++latched;
    }
    assert(submitted == snapshot.displayed_frames && latched == submitted);
    if (argc == 3) assert(overlapped > 0); /* Decode runs while the prior frame scans. */
    post(PLAYER_SESSION_STOP, 0, NULL);
    assert(snapshot.state == PLAYER_SESSION_IDLE && !allocations);
    free(avi);
    puts("session lifecycle: WAV/AVI pause, EOF, final frame, stop ordering and pinned failures passed");
    return 0;
}

#include "player_measurement.h"
#include "player_diagnostics.h"
#include "player_audio_client.h"
#include "player_session.h"
#include "platform_runtime.h"
#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#ifdef PLAYER_MEASUREMENT_TEST_CAPACITY
_Static_assert(PLAYER_MEASUREMENT_TEST_CAPACITY > 0 &&
               PLAYER_MEASUREMENT_TEST_CAPACITY <= PLAYER_MEASUREMENT_CAPACITY, "test capacity");
#define RECORD_CAPACITY PLAYER_MEASUREMENT_TEST_CAPACITY
#else
#define RECORD_CAPACITY PLAYER_MEASUREMENT_CAPACITY
#endif

enum counter_index {
    C_SAMPLES, C_DECODED, C_DISPLAYED, C_DROPPED, C_LATE, C_UNDERRUNS,
    C_WRITTEN, C_COMPLETED, C_DMA_ERRORS, C_DMA_TIMEOUTS, C_QUEUE_UNDER, C_FIFO_UNDER,
    C_DIAG_TX, C_DIAG_REMOTE_RX, C_DIAG_EVENT_TX, C_DIAG_HOST_RX,
    C_AUDIO_TX, C_AUDIO_REMOTE_RX, C_AUDIO_EVENT_TX, C_AUDIO_HOST_RX, C_REJECTED, C_COUNT
};
static const char *const counter_names[C_COUNT] = {
    "elapsed_samples", "decoded_frames", "displayed_frames", "dropped_frames", "late_frames", "audio_underruns",
    "audio_written_samples", "audio_completed_samples", "dma_errors", "dma_timeouts", "queue_underruns", "fifo_underruns",
    "diag_tx", "diag_remote_rx", "diag_event_tx", "diag_host_rx",
    "audio_tx", "audio_remote_rx", "audio_event_tx", "audio_host_rx", "rejected"
};
typedef struct {
    uint64_t counter_end;
    uint32_t counter_hz, window_us, heap_free, heap_min, heap_total;
    uint16_t busy_bp;
    uint8_t valid, heap_min_kind;
} measurement_cpu;
typedef struct {
    uint32_t elapsed_ms, session, audio_session, amp_session, flags, state;
    measurement_cpu cpu[2];
    uint32_t counter[C_COUNT];
    uint32_t total_samples, max_late_us, ring_samples, pending_samples;
    uint32_t video_packets, audio_packets, queued_pcm_samples, pinned;
    int32_t session_error, audio_error;
} measurement_record;

static measurement_record records[RECORD_CAPACITY], previous;
static player_session_snapshot_t session_view; /* Never put this 34KiB view on stack. */
static player_diagnostics_report cpu_view;
static player_audio_client_stats audio_view;
static player_measurement_info info;
static uint64_t start_us, last_us, ended_us;
static uint64_t cpu_weight[2], cpu_busy_weight[2], totals[C_COUNT];
static uint64_t previous_cpu_end[2];
static uint32_t previous_cpu_sequence[2];
static bool observed_playback;

static uint32_t milliseconds(uint64_t elapsed)
{
    return elapsed / 1000u > UINT32_MAX ? UINT32_MAX : (uint32_t)(elapsed / 1000u);
}

static void capture_cpu(measurement_cpu *out, const player_cpu_window *window,
                         uint32_t sequence, unsigned core, uint32_t *flags)
{
    out->counter_end = window->counter_end;
    out->counter_hz = window->counter_hz;
    out->window_us = window->window_us <= UINT32_MAX ? (uint32_t)window->window_us : 0;
    out->heap_free = window->heap_free_bytes;
    out->heap_min = window->heap_min_free_bytes;
    out->heap_total = window->heap_total_bytes;
    out->heap_min_kind = (uint8_t)window->heap_min_kind;
    bool fresh = sequence && (sequence != previous_cpu_sequence[core] ||
                              window->counter_end != previous_cpu_end[core]);
    out->valid = fresh && window->valid && window->window_us >= PLAYER_CPU_WINDOW_US &&
                 out->window_us && window->busy_basis_points <= 10000u;
    out->busy_bp = out->valid ? (uint16_t)window->busy_basis_points : 0;
    if (out->valid) {
        *flags |= core ? PLAYER_MEASURE_RISCV_FRESH : PLAYER_MEASURE_MIPS_FRESH;
        previous_cpu_sequence[core] = sequence;
        previous_cpu_end[core] = window->counter_end;
    }
}

static void capture(measurement_record *record, uint64_t now)
{
    memset(record, 0, sizeof(*record));
    record->elapsed_ms = milliseconds(now - start_us);
    if (player_session_snapshot(&session_view)) record->flags |= PLAYER_MEASURE_SNAPSHOT_INVALID;
    player_audio_client_get_status(&audio_view);
    memset(&cpu_view, 0, sizeof(cpu_view));
    (void)player_diagnostics_latest(&cpu_view);
    record->session = session_view.session_id;
    record->audio_session = audio_view.media_session_id;
    record->amp_session = cpu_view.amp_session;
    record->state = (uint32_t)session_view.state;
    record->session_error = session_view.error_code;
    record->audio_error = audio_view.last_error;
    record->pinned = session_view.resources_pinned;
    capture_cpu(&record->cpu[0], &cpu_view.mips_cpu, cpu_view.mips_window_sequence, 0, &record->flags);
    if (cpu_view.riscv_running) {
        record->flags |= PLAYER_MEASURE_RISCV_RUNNING;
        capture_cpu(&record->cpu[1], &cpu_view.riscv_cpu, cpu_view.riscv_window_sequence, 1, &record->flags);
    }
    record->counter[C_SAMPLES] = session_view.elapsed_samples;
    record->counter[C_DECODED] = session_view.decoded_frames;
    record->counter[C_DISPLAYED] = session_view.displayed_frames;
    record->counter[C_DROPPED] = session_view.dropped_frames;
    record->counter[C_LATE] = session_view.late_frames;
    record->counter[C_UNDERRUNS] = session_view.audio_underruns;
    record->counter[C_WRITTEN] = audio_view.written_samples;
    record->counter[C_COMPLETED] = audio_view.completed_samples;
    record->counter[C_DMA_ERRORS] = audio_view.dma_errors;
    record->counter[C_DMA_TIMEOUTS] = audio_view.dma_timeouts;
    record->counter[C_QUEUE_UNDER] = audio_view.queue_underruns;
    record->counter[C_FIFO_UNDER] = audio_view.fifo_underruns;
    record->counter[C_DIAG_TX] = cpu_view.host_commands_sent;
    record->counter[C_DIAG_REMOTE_RX] = cpu_view.remote_commands_received;
    record->counter[C_DIAG_EVENT_TX] = cpu_view.remote_events_sent;
    record->counter[C_DIAG_HOST_RX] = cpu_view.host_events_received;
    record->counter[C_AUDIO_TX] = cpu_view.audio_commands_sent;
    record->counter[C_AUDIO_REMOTE_RX] = cpu_view.audio_commands_received;
    record->counter[C_AUDIO_EVENT_TX] = cpu_view.audio_events_sent;
    record->counter[C_AUDIO_HOST_RX] = cpu_view.audio_events_received;
    record->counter[C_REJECTED] = cpu_view.remote_rejected_messages;
    record->total_samples = session_view.total_samples;
    record->max_late_us = session_view.max_late_us;
    record->video_packets = session_view.video_queue_packets;
    record->audio_packets = session_view.audio_queue_packets;
    record->queued_pcm_samples = session_view.queued_pcm_samples;
    record->ring_samples = cpu_view.pcm_written_samples - cpu_view.pcm_copied_samples;
    if (record->ring_samples > PLAYER_AMP_PCM_SAMPLES) {
        record->ring_samples = 0;
        record->flags |= PLAYER_MEASURE_RING_INVALID;
    }
    if (audio_view.completed_samples <= audio_view.written_samples)
        record->pending_samples = audio_view.written_samples - audio_view.completed_samples;
    else record->flags |= PLAYER_MEASURE_AUDIO_INVALID;
}

static uint32_t scope(const measurement_record *record, unsigned counter)
{
    return counter < C_WRITTEN ? record->session :
           counter < C_DIAG_TX ? record->audio_session : record->amp_session;
}

static void accumulate(measurement_record *record)
{
    for (unsigned i = 0; i < C_COUNT; ++i) {
        bool same = scope(record, i) == scope(&previous, i);
        uint32_t delta = same ? record->counter[i] - previous.counter[i] : record->counter[i];
        if (!same) record->flags |= PLAYER_MEASURE_SCOPE_CHANGED;
        else if (record->counter[i] < previous.counter[i]) record->flags |= PLAYER_MEASURE_COUNTER_WRAP;
        totals[i] += delta;
    }
    for (unsigned core = 0; core < 2; ++core) {
        const measurement_cpu *cpu = &record->cpu[core];
        if (!cpu->valid) continue;
        cpu_weight[core] += cpu->window_us;
        cpu_busy_weight[core] += (uint64_t)cpu->busy_bp * cpu->window_us;
        if (core) {
            ++info.riscv_windows;
            if (cpu->heap_free < info.riscv_min_heap_free) info.riscv_min_heap_free = cpu->heap_free;
        } else {
            ++info.mips_windows;
            if (cpu->heap_free < info.mips_min_heap_free) info.mips_min_heap_free = cpu->heap_free;
        }
    }
#define MAXIMUM(field, value) do { if ((value) > info.field) info.field = (value); } while (0)
    MAXIMUM(max_ring_samples, record->ring_samples);
    MAXIMUM(max_pending_samples, record->pending_samples);
    MAXIMUM(max_video_packets, record->video_packets);
    MAXIMUM(max_audio_packets, record->audio_packets);
    MAXIMUM(max_queued_pcm_samples, record->queued_pcm_samples);
    MAXIMUM(max_late_us, record->max_late_us);
#undef MAXIMUM
    previous = *record;
}

static bool playback_observed(void)
{
    return session_view.state == PLAYER_SESSION_LOADING || session_view.state == PLAYER_SESSION_PLAYING ||
           session_view.state == PLAYER_SESSION_PAUSED;
}

int player_measurement_begin(const char *label, uint32_t seconds)
{
    if (info.active) return -EBUSY;
    if (!label || !label[0] || strlen(label) >= sizeof(info.label) ||
        strchr(label, '\r') || strchr(label, '\n') || !seconds || seconds > PLAYER_MEASUREMENT_MAX_SECONDS)
        return -EINVAL;
    memset(&info, 0, sizeof(info));
    memcpy(info.label, label, strlen(label) + 1u);
    info.target_seconds = seconds;
    info.record_capacity = RECORD_CAPACITY;
    info.record_bytes = (uint32_t)sizeof(measurement_record);
    info.mips_min_heap_free = info.riscv_min_heap_free = UINT32_MAX;
    memset(totals, 0, sizeof(totals));
    memset(cpu_weight, 0, sizeof(cpu_weight));
    memset(cpu_busy_weight, 0, sizeof(cpu_busy_weight));
    memset(previous_cpu_sequence, 0, sizeof(previous_cpu_sequence));
    memset(previous_cpu_end, 0, sizeof(previous_cpu_end));
    start_us = last_us = platform_time_us();
    ended_us = start_us;
    capture(&previous, start_us); /* Baseline only; does not count the cached CPU window. */
    observed_playback = playback_observed();
    info.active = 1;
    return 0;
}

static int record_and_maybe_end(uint64_t now, enum player_measurement_end reason)
{
    if (info.record_count == RECORD_CAPACITY) reason = PLAYER_MEASURE_FULL;
    else {
        measurement_record *record = &records[info.record_count];
        capture(record, now);
        if (playback_observed()) observed_playback = true;
        if (reason == PLAYER_MEASURE_NONE && observed_playback) {
            if (session_view.state == PLAYER_SESSION_ERROR || record->session_error || record->audio_error)
                reason = PLAYER_MEASURE_ERROR;
            else if (session_view.state == PLAYER_SESSION_ENDED) reason = PLAYER_MEASURE_EOF;
            else if (session_view.state == PLAYER_SESSION_IDLE) reason = PLAYER_MEASURE_SESSION_STOP;
        }
        if (reason == PLAYER_MEASURE_NONE && now - start_us >= (uint64_t)info.target_seconds * 1000000u)
            reason = PLAYER_MEASURE_DURATION;
        if (reason == PLAYER_MEASURE_NONE && info.record_count + 1u == RECORD_CAPACITY)
            reason = PLAYER_MEASURE_FULL;
        if (reason != PLAYER_MEASURE_NONE) record->flags |= PLAYER_MEASURE_FINAL;
        accumulate(record);
        ++info.record_count;
        last_us = now;
    }
    if (reason == PLAYER_MEASURE_NONE) return 0;
    info.reason = (uint32_t)reason;
    info.active = 0;
    ended_us = now;
    return 1;
}

int player_measurement_step(void)
{
    if (!info.active) return 0;
    uint64_t now = platform_time_us();
    if (now - last_us < 1000000u && now - start_us < (uint64_t)info.target_seconds * 1000000u) return 0;
    return record_and_maybe_end(now, PLAYER_MEASURE_NONE);
}

int player_measurement_stop(void)
{
    if (!info.active) return 0;
    uint64_t now = platform_time_us();
    if (info.record_count && now == last_us) {
        records[info.record_count - 1u].flags |= PLAYER_MEASURE_FINAL;
        info.active = 0;
        info.reason = PLAYER_MEASURE_MANUAL;
        ended_us = now;
        return 1;
    }
    return record_and_maybe_end(now, PLAYER_MEASURE_MANUAL);
}

void player_measurement_get_info(player_measurement_info *out)
{
    if (!out) return;
    *out = info;
    out->elapsed_ms = milliseconds((info.active ? platform_time_us() : ended_us) - start_us);
    out->mips_mean_busy_bp = cpu_weight[0] ? (uint32_t)(cpu_busy_weight[0] / cpu_weight[0]) : 0;
    out->riscv_mean_busy_bp = cpu_weight[1] ? (uint32_t)(cpu_busy_weight[1] / cpu_weight[1]) : 0;
    if (!out->mips_windows) out->mips_min_heap_free = 0;
    if (!out->riscv_windows) out->riscv_min_heap_free = 0;
    out->elapsed_samples = totals[C_SAMPLES];
    out->decoded_frames = totals[C_DECODED];
    out->displayed_frames = totals[C_DISPLAYED];
    out->dropped_frames = totals[C_DROPPED];
    out->late_frames = totals[C_LATE];
    out->audio_underruns = totals[C_UNDERRUNS];
    out->dma_errors = totals[C_DMA_ERRORS];
    out->dma_timeouts = totals[C_DMA_TIMEOUTS];
}

static const char *reason_name(uint32_t reason)
{
    static const char *const names[] = {"none", "manual", "duration", "eof", "session-stop", "error", "full"};
    return reason <= PLAYER_MEASURE_FULL ? names[reason] : "unknown";
}

void player_measurement_print_summary(void)
{
    player_measurement_info current;
    player_measurement_get_info(&current);
    printf("MEASURE_SUMMARY label=%s active=%lu reason=%s records=%lu/%lu record_bytes=%lu elapsed_ms=%lu target_s=%lu mips_windows=%lu mips_busy_bp=%lu rv_windows=%lu rv_busy_bp=%lu mips_min_heap=%lu rv_min_heap=%lu samples=%llu decoded=%llu displayed=%llu dropped=%llu late=%llu max_late_us=%lu underruns=%llu dma_errors=%llu dma_timeouts=%llu max_shared_ring_samples=%lu max_pending_valid_samples=%lu max_video_packets=%lu max_audio_packets=%lu max_queued_pcm_samples=%lu\n",
        current.label, (unsigned long)current.active, reason_name(current.reason),
        (unsigned long)current.record_count, (unsigned long)current.record_capacity,
        (unsigned long)current.record_bytes, (unsigned long)current.elapsed_ms, (unsigned long)current.target_seconds,
        (unsigned long)current.mips_windows, (unsigned long)current.mips_mean_busy_bp,
        (unsigned long)current.riscv_windows, (unsigned long)current.riscv_mean_busy_bp,
        (unsigned long)current.mips_min_heap_free, (unsigned long)current.riscv_min_heap_free,
        (unsigned long long)current.elapsed_samples, (unsigned long long)current.decoded_frames,
        (unsigned long long)current.displayed_frames, (unsigned long long)current.dropped_frames,
        (unsigned long long)current.late_frames, (unsigned long)current.max_late_us,
        (unsigned long long)current.audio_underruns, (unsigned long long)current.dma_errors,
        (unsigned long long)current.dma_timeouts, (unsigned long)current.max_ring_samples,
        (unsigned long)current.max_pending_samples, (unsigned long)current.max_video_packets,
        (unsigned long)current.max_audio_packets, (unsigned long)current.max_queued_pcm_samples);
}

static void csv_label(void)
{
    printf("\"");
    for (const char *p = info.label; *p; ++p) {
        if (*p == '"') printf("\"");
        printf("%c", *p);
    }
    printf("\"");
}

int player_measurement_dump_csv(void (*pump)(void))
{
    if (info.active) return -EBUSY;
    printf("label,elapsed_ms,session,audio_session,amp_session,flags,state");
    for (unsigned core = 0; core < 2; ++core) {
        const char *name = core ? "rv" : "mips";
        printf(",%s_valid,%s_counter_end,%s_counter_hz,%s_window_us,%s_busy_bp,%s_heap_free,%s_heap_min,%s_heap_total,%s_heap_min_kind",
               name, name, name, name, name, name, name, name, name);
    }
    for (unsigned i = 0; i < C_COUNT; ++i) printf(",%s", counter_names[i]);
    printf(",total_samples,max_late_us,shared_ring_samples,pending_valid_samples,video_packets,audio_packets,queued_pcm_samples,pinned,session_error,audio_error\n");
    for (unsigned row = 0; row < info.record_count; ++row) {
        if (pump) pump();
        const measurement_record *record = &records[row];
        csv_label();
        printf(",%lu,%lu,%lu,%lu,%lu,%lu", (unsigned long)record->elapsed_ms,
               (unsigned long)record->session, (unsigned long)record->audio_session,
               (unsigned long)record->amp_session, (unsigned long)record->flags, (unsigned long)record->state);
        for (unsigned core = 0; core < 2; ++core) {
            const measurement_cpu *cpu = &record->cpu[core];
            printf(",%u,%llu,%lu,%lu,%u,%lu,%lu,%lu,%u", (unsigned)cpu->valid,
                (unsigned long long)cpu->counter_end, (unsigned long)cpu->counter_hz,
                (unsigned long)cpu->window_us, (unsigned)cpu->busy_bp, (unsigned long)cpu->heap_free,
                (unsigned long)cpu->heap_min, (unsigned long)cpu->heap_total, (unsigned)cpu->heap_min_kind);
        }
        for (unsigned i = 0; i < C_COUNT; ++i) printf(",%lu", (unsigned long)record->counter[i]);
        printf(",%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%ld,%ld\n", (unsigned long)record->total_samples,
            (unsigned long)record->max_late_us, (unsigned long)record->ring_samples,
            (unsigned long)record->pending_samples, (unsigned long)record->video_packets,
            (unsigned long)record->audio_packets, (unsigned long)record->queued_pcm_samples,
            (unsigned long)record->pinned, (long)record->session_error, (long)record->audio_error);
    }
    return 0;
}

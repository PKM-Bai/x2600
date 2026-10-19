/* Real recorder; replace only source snapshots, time and UART printf. */
#include "player_measurement.h"
#include "player_diagnostics.h"
#include "player_audio_client.h"
#include "player_session.h"
#include <assert.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

static uint64_t now;
static player_diagnostics_report report;
static player_session_snapshot_t snapshot;
static player_audio_client_stats audio;
static char uart[1024 * 1024];
static unsigned uart_size, print_calls, pump_calls;
static player_measurement_info current;
static void pump(void) { ++pump_calls; }

int measurement_test_printf(const char *format, ...)
{
    va_list arguments;
    va_start(arguments, format);
    int count = vsnprintf(uart + uart_size, sizeof(uart) - uart_size, format, arguments);
    va_end(arguments);
    assert(count >= 0 && (unsigned)count < sizeof(uart) - uart_size);
    uart_size += (unsigned)count;
    ++print_calls;
    return count;
}
uint64_t platform_time_us(void) { return now; }
int player_diagnostics_latest(player_diagnostics_report *out) { *out = report; return report.mips_cpu.valid ? 1 : 0; }
int player_session_snapshot(player_session_snapshot_t *out) { *out = snapshot; return 0; }
void player_audio_client_get_status(player_audio_client_stats *out) { *out = audio; }

static void reset_sources(void)
{
    (void)player_measurement_stop();
    memset(&report, 0, sizeof(report));
    memset(&snapshot, 0, sizeof(snapshot));
    memset(&audio, 0, sizeof(audio));
    memset(uart, 0, sizeof(uart));
    uart_size = print_calls = pump_calls = 0;
    now = 0;
    snapshot.session_id = audio.media_session_id = report.amp_session = 1;
    snapshot.state = PLAYER_SESSION_PLAYING;
    snapshot.total_samples = audio.total_samples = 1000000000u;
    report.mips_window_sequence = report.riscv_window_sequence = 1;
    report.riscv_running = 1;
    report.mips_cpu.valid = report.riscv_cpu.valid = 1;
    report.mips_cpu.counter_hz = report.riscv_cpu.counter_hz = 1000000;
    report.mips_cpu.counter_end = report.riscv_cpu.counter_end = 5000000;
    report.mips_cpu.window_us = report.riscv_cpu.window_us = 1000000;
    report.mips_cpu.busy_basis_points = 1000;
    report.riscv_cpu.busy_basis_points = 2000;
    report.mips_cpu.heap_free_bytes = 20000000;
    report.riscv_cpu.heap_free_bytes = 100000;
}

static void advance(unsigned seconds)
{
    now += (uint64_t)seconds * 1000000u;
    snapshot.elapsed_samples += seconds * 48000u;
    snapshot.decoded_frames += seconds * 30u;
    snapshot.displayed_frames += seconds * 30u;
    audio.written_samples += seconds * 48000u;
    audio.completed_samples += seconds * 48000u;
    report.mips_window_sequence += 1u;
    report.riscv_window_sequence += 1u;
    report.mips_cpu.counter_end += (uint64_t)seconds * 1000000u;
    report.riscv_cpu.counter_end += (uint64_t)seconds * 1000000u;
    report.mips_cpu.window_us = report.riscv_cpu.window_us = (uint64_t)seconds * 1000000u;
}

static void duration_and_cpu(void)
{
    reset_sources();
    assert(player_measurement_begin(NULL, 1) == -EINVAL);
    assert(player_measurement_begin("", 1) == -EINVAL);
    assert(player_measurement_begin("x", 0) == -EINVAL);
    assert(player_measurement_begin("x", 1801) == -EINVAL);
    assert(player_measurement_begin("amp,\"quiet\"", 4) == 0);
    assert(player_measurement_begin("other", 1) == -EBUSY);
    assert(player_measurement_dump_csv(pump) == -EBUSY && print_calls == 0 && pump_calls == 0);
    now = 999999;
    assert(player_measurement_step() == 0);
    player_measurement_get_info(&current);
    assert(current.record_count == 0);
    now = 0;
    advance(1);
    snapshot.video_queue_packets = 3;
    snapshot.audio_queue_packets = 4;
    snapshot.queued_pcm_samples = 9000;
    audio.written_samples += 960;
    report.pcm_written_samples = 2000;
    report.pcm_copied_samples = 1000;
    assert(player_measurement_step() == 0);
    /* A stale cached CPU window must never be weighted twice. */
    now += 1000000;
    assert(player_measurement_step() == 0);
    advance(1);
    report.mips_cpu.busy_basis_points = 3000;
    report.riscv_running = 0;
    assert(player_measurement_step() == 0);
    advance(1);
    assert(player_measurement_step() == 1);
    player_measurement_get_info(&current);
    assert(!current.active && current.reason == PLAYER_MEASURE_DURATION && current.record_count == 4);
    assert(current.mips_windows == 3 && current.mips_mean_busy_bp == 2333);
    assert(current.riscv_windows == 1 && current.riscv_mean_busy_bp == 2000);
    assert(current.max_ring_samples == 1000 && current.max_pending_samples == 960);
    assert(current.max_video_packets == 3 && current.max_audio_packets == 4 && current.max_queued_pcm_samples == 9000);
    assert(print_calls == 0);
    player_measurement_print_summary();
    assert(strstr(uart, "reason=duration") && strstr(uart, "mips_busy_bp=2333"));
    uart_size = print_calls = 0;
    uart[0] = 0;
    assert(player_measurement_dump_csv(pump) == 0 && pump_calls == 4);
    assert(strstr(uart, "\"amp,\"\"quiet\"\"\",1000,") && strstr(uart, "shared_ring_samples,pending_valid_samples"));
    unsigned lines = 0;
    for (const char *p = uart; *p; ++p) if (*p == '\n') ++lines;
    assert(lines == 5);
}

static void wrap_and_scopes(void)
{
    reset_sources();
    now = UINT64_MAX - 499999u;
    snapshot.displayed_frames = UINT32_MAX - 2u;
    report.host_commands_sent = UINT32_MAX - 1u;
    assert(player_measurement_begin("wrap", 20) == 0);
    advance(1); /* Both source time and uint32 frame counter wrap. */
    report.host_commands_sent = 3;
    assert(player_measurement_step() == 0);
    player_measurement_get_info(&current);
    assert(current.elapsed_ms == 1000 && current.displayed_frames == 30);
    snapshot.session_id = 2;
    audio.media_session_id = 2;
    report.amp_session = 2;
    snapshot.elapsed_samples = 10;
    snapshot.decoded_frames = snapshot.displayed_frames = 4;
    audio.written_samples = 100;
    audio.completed_samples = 50;
    report.host_commands_sent = 2;
    now += 1000000;
    assert(player_measurement_step() == 0);
    player_measurement_get_info(&current);
    assert(current.displayed_frames == 34 && current.elapsed_samples == 48010);
    assert(player_measurement_stop() == 1 && player_measurement_stop() == 0);
    player_measurement_get_info(&current);
    assert(current.record_count == 2 && current.reason == PLAYER_MEASURE_MANUAL && !print_calls);
    assert(player_measurement_dump_csv(NULL) == 0);
    assert(strstr(uart, ",32,2,") || strstr(uart, ",46,2,")); /* wrap bit, plus CPU flags */
    assert(strstr(uart, ",25,2,")); /* final + scope change + RV running, stale CPU */
}

static void eof_error_and_stop(void)
{
    reset_sources();
    assert(player_measurement_begin("eof", 1800) == 0);
    advance(1);
    snapshot.state = PLAYER_SESSION_ENDED;
    assert(player_measurement_step() == 1);
    player_measurement_get_info(&current);
    assert(current.reason == PLAYER_MEASURE_EOF && current.record_count == 1 && !print_calls);
    reset_sources();
    snapshot.state = PLAYER_SESSION_IDLE;
    assert(player_measurement_begin("before-play", 1800) == 0);
    advance(1);
    assert(player_measurement_step() == 0); /* Idle before first playback is valid. */
    snapshot.state = PLAYER_SESSION_PLAYING;
    advance(1);
    assert(player_measurement_step() == 0);
    snapshot.state = PLAYER_SESSION_IDLE;
    advance(1);
    assert(player_measurement_step() == 1);
    player_measurement_get_info(&current);
    assert(current.reason == PLAYER_MEASURE_SESSION_STOP);
    reset_sources();
    assert(player_measurement_begin("fault", 1800) == 0);
    advance(1);
    snapshot.state = PLAYER_SESSION_ERROR;
    audio.last_error = -42;
    audio.dma_errors = 1;
    assert(player_measurement_step() == 1);
    player_measurement_get_info(&current);
    assert(current.reason == PLAYER_MEASURE_ERROR && current.dma_errors == 1);
    reset_sources();
    assert(player_measurement_begin("partial", 1800) == 0);
    now = 250000;
    assert(player_measurement_stop() == 1);
    player_measurement_get_info(&current);
    assert(current.record_count == 1 && current.elapsed_ms == 250 && !print_calls);
}

static void full_capacity(void)
{
    reset_sources();
    assert(player_measurement_begin("full", 1800) == 0);
    player_measurement_get_info(&current);
    unsigned capacity = current.record_capacity;
    for (unsigned i = 0; i < capacity; ++i) {
        advance(1);
        assert(player_measurement_step() == (i + 1u == capacity ? 1 : 0));
    }
    player_measurement_get_info(&current);
    assert(current.reason == PLAYER_MEASURE_FULL && current.record_count == capacity);
    for (unsigned i = 0; i < 10; ++i) { advance(1); assert(player_measurement_step() == 0); }
    player_measurement_get_info(&current);
    assert(current.record_count == capacity && !print_calls);
}

static void thirty_minutes(void)
{
    reset_sources();
    assert(player_measurement_begin("1800s", 1800) == 0);
    for (unsigned i = 1; i <= 1800; ++i) {
        advance(1);
        assert(player_measurement_step() == (i == 1800 ? 1 : 0));
    }
    player_measurement_get_info(&current);
    assert(current.reason == PLAYER_MEASURE_DURATION && current.record_count == 1800);
    assert(current.record_capacity == 1860 && current.elapsed_ms == 1800000);
    assert(current.displayed_frames == 54000 && current.elapsed_samples == 86400000 && !print_calls);
    assert(player_measurement_dump_csv(pump) == 0 && pump_calls == 1800);
}

int main(void)
{
#ifdef PLAYER_MEASUREMENT_TEST_CAPACITY
    full_capacity();
#else
    duration_and_cpu();
    wrap_and_scopes();
    eof_error_and_stop();
    thirty_minutes();
#endif
    puts("PASS production measurement: quiet RAM capture, cached CPU dedup, counters/time wrap, scopes, CSV, EOF/error/STOP, duration/capacity");
    return 0;
}

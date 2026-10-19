#include "player_diagnostics.h"
#include "player_amp.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static struct player_amp_shared test_shared;
static player_cpu_raw local_raw;
static unsigned capture_calls;
static int amp_present = 1;
int player_cpu_diagnostics_capture(player_cpu_raw *raw)
{
    *raw = local_raw; ++capture_calls; return 1;
}
const volatile struct player_amp_shared *player_amp_state(void)
{ return amp_present ? &test_shared : NULL; }

/* @PRODUCTION_DIAGNOSTICS@ */

int main(void)
{
    local_raw.counter_hz = 1000000;
    local_raw.heap_free_bytes = 900; local_raw.heap_min_free_bytes = 800;
    local_raw.heap_total_bytes = 1000; local_raw.heap_min_kind = PLAYER_HEAP_MIN_SAMPLED;
    player_amp_initialize(&test_shared, 1, PLAYER_AUDIO_OWNER_RISCV, 1, 4096);
    test_shared.host_state = test_shared.remote_state = PLAYER_AMP_RUNNING;
    player_cpu_raw remote_raw = local_raw;
    remote_raw.heap_min_kind = PLAYER_HEAP_MIN_ALLOCATOR;
    remote_raw.counter = 100000;
    player_cpu_diagnostics_publish(&test_shared.remote_diagnostics, &remote_raw, 1);
    player_diagnostics_reset();
    unsigned before = capture_calls;
    player_diagnostics_report report;
    assert(player_diagnostics_latest(&report) == 0 && capture_calls == before);
    local_raw.counter = 500000; local_raw.idle_counter = 400000;
    assert(player_diagnostics_poll(&report) == 0 && !report.mips_cpu.valid);
    local_raw.counter = 1000000; local_raw.idle_counter = 750000;
    remote_raw.counter += 1000000; remote_raw.idle_counter += 500000;
    player_cpu_diagnostics_publish(&test_shared.remote_diagnostics, &remote_raw, 2);
    assert(player_diagnostics_poll(&report) == 1);
    assert(report.mips_window_sequence == 1 && report.riscv_window_sequence == 1);
    assert(report.mips_cpu.busy_basis_points == 2500 && report.riscv_cpu.busy_basis_points == 5000);
    assert(report.mips_cpu.counter_end == 1000000 && report.riscv_cpu.counter_end == 1100000);
    test_shared.host_to_remote.write_pos = 26; test_shared.host_to_remote.read_pos = 25;
    test_shared.remote_to_host.write_pos = 24; test_shared.remote_to_host.read_pos = 23;
    test_shared.audio_host_to_remote.write_pos = 12; test_shared.audio_host_to_remote.read_pos = 11;
    test_shared.audio_remote_to_host.write_pos = 10; test_shared.audio_remote_to_host.read_pos = 9;
    test_shared.pcm_write_pos = 31; test_shared.pcm_read_pos = 13;
    test_shared.remote_rejected_messages = 2;
    assert(player_diagnostics_poll(&report) == 0 && report.mips_window_sequence == 1);
    assert(report.host_commands_sent == 26 && report.remote_commands_received == 25);
    assert(report.remote_events_sent == 24 && report.host_events_received == 23);
    assert(report.audio_commands_sent == 12 && report.audio_commands_received == 11);
    assert(report.audio_events_sent == 10 && report.audio_events_received == 9);
    assert(report.pcm_written_samples == 31 && report.pcm_copied_samples == 13);
    assert(report.remote_rejected_messages == 2);
    before = capture_calls;
    assert(player_diagnostics_latest(&report) == 1);
    assert(player_diagnostics_latest(&report) == 1 && capture_calls == before);
    player_diagnostics_print();
    assert(capture_calls == before); /* metrics must not consume another window. */
    local_raw.counter += 1000000; local_raw.idle_counter += 500000;
    assert(player_diagnostics_poll(&report) == 1 && report.mips_window_sequence == 2);
    assert(report.riscv_window_sequence == 1 && report.mips_cpu.busy_basis_points == 5000);
    test_shared.remote_state = PLAYER_AMP_STOPPED;
    assert(player_diagnostics_poll(&report) == 0 && !report.riscv_cpu.valid && !report.riscv_running);
    amp_present = 0;
    assert(player_diagnostics_poll(&report) == 0 && !report.riscv_cpu.valid);
    --local_raw.counter;
    assert(player_diagnostics_poll(&report) == -1 && !report.mips_cpu.valid);
    assert(player_diagnostics_latest(NULL) == -1 && player_diagnostics_poll(NULL) == -1);
    puts("PASS diagnostics production poll/latest: single producer, non-consuming readers/metrics, independent windows, actual ring/PCM counters, reset-core unavailable");
    return 0;
}

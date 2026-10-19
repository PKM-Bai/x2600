#include "player_diagnostics.h"
#include "player_platform.h"
#include <stdio.h>
#include <string.h>

static player_cpu_sampler mips_sampler, riscv_sampler;
static player_diagnostics_report latest;
static uint32_t riscv_boot_session, riscv_sample_sequence;

static void read_remote(void)
{
    const volatile struct player_amp_shared *shared = player_amp_state();
    if (!shared || !player_amp_header_valid(shared)) {
        latest.riscv_running = 0;
        latest.riscv_cpu.valid = 0;
        riscv_sampler.initialized = 0;
        return;
    }
    if (riscv_boot_session != shared->session) {
        riscv_boot_session = shared->session;
        riscv_sample_sequence = 0;
        riscv_sampler.initialized = 0;
        latest.riscv_cpu.valid = 0;
    }
    latest.amp_session = shared->session;
    latest.riscv_running = shared->host_state == PLAYER_AMP_RUNNING &&
                           shared->remote_state == PLAYER_AMP_RUNNING;
    player_amp_barrier();
    latest.host_commands_sent = shared->host_to_remote.write_pos;
    latest.remote_commands_received = shared->host_to_remote.read_pos;
    latest.remote_events_sent = shared->remote_to_host.write_pos;
    latest.host_events_received = shared->remote_to_host.read_pos;
    latest.audio_commands_sent = shared->audio_host_to_remote.write_pos;
    latest.audio_commands_received = shared->audio_host_to_remote.read_pos;
    latest.audio_events_sent = shared->audio_remote_to_host.write_pos;
    latest.audio_events_received = shared->audio_remote_to_host.read_pos;
    latest.pcm_written_samples = shared->pcm_write_pos;
    latest.pcm_copied_samples = shared->pcm_read_pos;
    latest.remote_rejected_messages = shared->remote_rejected_messages;
    if (!latest.riscv_running) {
        latest.riscv_cpu.valid = 0;
        riscv_sampler.initialized = 0;
        return;
    }
    player_cpu_raw raw;
    uint32_t sequence;
    if (player_cpu_diagnostics_read(&shared->remote_diagnostics, &raw, &sequence) &&
        sequence != riscv_sample_sequence) {
        riscv_sample_sequence = sequence;
        int result = player_cpu_diagnostics_window(&riscv_sampler, &raw, &latest.riscv_cpu);
        if (result < 0)
            latest.riscv_cpu.valid = 0;
        else if (result == 1 && ++latest.riscv_window_sequence == 0)
            ++latest.riscv_window_sequence;
    }
}

void player_diagnostics_reset(void)
{
    memset(&mips_sampler, 0, sizeof(mips_sampler));
    memset(&riscv_sampler, 0, sizeof(riscv_sampler));
    memset(&latest, 0, sizeof(latest));
    riscv_boot_session = riscv_sample_sequence = 0;
    player_cpu_raw raw;
    if (player_cpu_diagnostics_capture(&raw))
        (void)player_cpu_diagnostics_window(&mips_sampler, &raw, &latest.mips_cpu);
    read_remote();
}

int player_diagnostics_poll(player_diagnostics_report *report)
{
    if (!report) return -1;
    read_remote();
    player_cpu_raw raw;
    int result = 0;
    if (player_cpu_diagnostics_capture(&raw)) {
        result = player_cpu_diagnostics_window(&mips_sampler, &raw, &latest.mips_cpu);
        if (result < 0) latest.mips_cpu.valid = 0;
        else if (result == 1 && ++latest.mips_window_sequence == 0)
            ++latest.mips_window_sequence;
    }
    *report = latest;
    return result;
}

int player_diagnostics_latest(player_diagnostics_report *report)
{
    if (!report) return -1;
    *report = latest;
    return latest.mips_cpu.valid ? 1 : 0;
}

void player_diagnostics_print(void)
{
    player_diagnostics_report report;
    if (player_diagnostics_latest(&report) != 1 && player_diagnostics_poll(&report) != 1) {
        printf("PLAYER_CPU valid=0 reason=window-not-ready min_window_us=1000000\n");
        return;
    }
    printf("PLAYER_CPU basis=scheduler-idle isr=charged-to-interrupted-task mips_valid=%lu mips_busy_bp=%lu mips_window_us=%llu mips_heap_free=%lu mips_heap_min=%lu mips_heap_total=%lu mips_heap_min_kind=%lu rv_running=%lu rv_valid=%lu rv_busy_bp=%lu rv_window_us=%llu rv_heap_free=%lu rv_heap_min=%lu rv_heap_total=%lu rv_heap_min_kind=%lu\n",
           (unsigned long)report.mips_cpu.valid, (unsigned long)report.mips_cpu.busy_basis_points,
           (unsigned long long)report.mips_cpu.window_us, (unsigned long)report.mips_cpu.heap_free_bytes,
           (unsigned long)report.mips_cpu.heap_min_free_bytes, (unsigned long)report.mips_cpu.heap_total_bytes,
           (unsigned long)report.mips_cpu.heap_min_kind, (unsigned long)report.riscv_running,
           (unsigned long)report.riscv_cpu.valid, (unsigned long)report.riscv_cpu.busy_basis_points,
           (unsigned long long)report.riscv_cpu.window_us, (unsigned long)report.riscv_cpu.heap_free_bytes,
           (unsigned long)report.riscv_cpu.heap_min_free_bytes, (unsigned long)report.riscv_cpu.heap_total_bytes,
           (unsigned long)report.riscv_cpu.heap_min_kind);
    printf("PLAYER_COMM amp_session=%lu diag_tx=%lu diag_remote_rx=%lu diag_event_tx=%lu diag_host_rx=%lu audio_tx=%lu audio_remote_rx=%lu audio_event_tx=%lu audio_host_rx=%lu pcm_written=%lu pcm_copied=%lu rejected=%lu pcm_scope=current-media-session\n",
           (unsigned long)report.amp_session, (unsigned long)report.host_commands_sent,
           (unsigned long)report.remote_commands_received, (unsigned long)report.remote_events_sent,
           (unsigned long)report.host_events_received, (unsigned long)report.audio_commands_sent,
           (unsigned long)report.audio_commands_received, (unsigned long)report.audio_events_sent,
           (unsigned long)report.audio_events_received, (unsigned long)report.pcm_written_samples,
           (unsigned long)report.pcm_copied_samples, (unsigned long)report.remote_rejected_messages);
}

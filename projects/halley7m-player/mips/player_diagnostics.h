#ifndef PLAYER_DIAGNOSTICS_H
#define PLAYER_DIAGNOSTICS_H

#include "player_cpu_diagnostics.h"

typedef struct player_diagnostics_report {
    player_cpu_window mips_cpu;
    player_cpu_window riscv_cpu;
    uint32_t mips_window_sequence;
    uint32_t riscv_window_sequence;
    uint32_t riscv_running;
    uint32_t amp_session;
    uint32_t host_commands_sent;
    uint32_t remote_events_sent;
    uint32_t remote_commands_received;
    uint32_t host_events_received;
    uint32_t audio_commands_sent;
    uint32_t audio_events_sent;
    uint32_t audio_commands_received;
    uint32_t audio_events_received;
    uint32_t pcm_written_samples;
    uint32_t pcm_copied_samples;
    uint32_t remote_rejected_messages;
} player_diagnostics_report;

/* Single media-owner task. Reset establishes a new measurement baseline;
 * subsequent poll returns 1 once >=1s of local counter time has elapsed,
 * 0 while collecting, or -1 for inconsistent counters. RV's window is its own
 * >=1s interval and is marked unavailable when held reset. Communication
 * counts are uint32 ring positions since the AMP boot session; PCM positions
 * belong to the current media session and reset only after confirmed STOP.
 */
void player_diagnostics_reset(void);
int player_diagnostics_poll(player_diagnostics_report *report);
/* Non-consuming copy for measurement/UI readers on the same owner task.
 * 1=valid MIPS window, 0=not yet available, -1=NULL. Sequences increment on
 * newly measured windows; counter_end is also an absolute source timestamp.
 */
int player_diagnostics_latest(player_diagnostics_report *report);
/* Print the cached valid report; poll only if no window is available yet. */
void player_diagnostics_print(void);

#endif

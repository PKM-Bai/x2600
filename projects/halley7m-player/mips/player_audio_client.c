#include "player_audio_client.h"
#include "player_platform.h"
#include "FreeRTOS.h"
#include "task.h"
#include <string.h>
#if PLAYER_AUDIO_OWNER == PLAYER_AUDIO_OWNER_MIPS
#include "../audio/player_audio.h"
#endif

#define COMMAND_TIMEOUT_US UINT64_C(2000000)

static player_audio_client_stats status;
static uint32_t next_media_session;
static int client_recovery_required;
#if PLAYER_AUDIO_OWNER == PLAYER_AUDIO_OWNER_RISCV
static volatile struct player_amp_shared *bus;
static uint32_t boot_session, command_sequence, event_sequence, event_completed_samples;
static uint32_t pending_sequence, pending_opcode, acknowledged_sequence;
static int protocol_broken, command_error;
static int remote_available(void);

static int fail_client(int error)
{
    status.state = PLAYER_AMP_AUDIO_FAULT;
    status.clock_valid = 0;
    status.last_error = error;
    return error;
}

static int break_protocol(int error)
{
    if (!protocol_broken && remote_available()) {
        /* Try once to end queued playback even when the reply path is broken.
         * This is not an ACK: storage remains pinned and restart is forbidden.
         */
        uint32_t sequence = command_sequence + 1u;
        if (!sequence) ++sequence;
        struct player_amp_message stop = {
            .session = status.media_session_id, .sequence = sequence,
            .opcode = PLAYER_AMP_AUDIO_CMD_STOP, .argument = {0, 0, 0, 0}
        };
        if (player_amp_send(&bus->audio_host_to_remote, &stop) == 1)
            command_sequence = sequence;
    }
    /* A lost/malformed transaction cannot prove DMA stopped. Keep the session
     * and its storage pinned; a board/core recovery must establish silence.
     */
    protocol_broken = 1;
    client_recovery_required = 1;
    player_subcore_force_silence();
    pending_opcode = pending_sequence = 0;
    return fail_client(error);
}

static int remote_available(void)
{
    return bus && player_amp_header_valid(bus) && bus->session == boot_session &&
           bus->host_state == PLAYER_AMP_RUNNING && bus->remote_state == PLAYER_AMP_RUNNING;
}

static int refresh_remote_stats(void)
{
    if (!bus || !player_amp_header_valid(bus) ||
        bus->audio_media_session_id != status.media_session_id) return 0;
    player_amp_barrier();
    status.dma_errors = bus->audio_dma_errors;
    status.dma_timeouts = bus->audio_dma_timeouts;
    status.queue_underruns = bus->audio_queue_underruns;
    status.fifo_underruns = bus->audio_fifo_underruns;
    status.underruns = bus->audio_underruns;
    status.clock_valid = protocol_broken || status.last_error ? 0 : bus->audio_clock_valid;
    uint32_t completed = bus->audio_completed_frames;
    if (completed < status.completed_samples || completed > status.written_samples) return -1;
    status.completed_samples = completed;
    return 0;
}

static int consume_event(const struct player_amp_message *event)
{
    uint32_t expected = event_sequence + 1u;
    if (!expected) ++expected;
    if (event->session != status.media_session_id || event->sequence != expected)
        return break_protocol(PLAYER_AUDIO_CLIENT_PROTOCOL);
    event_sequence = expected;
    if (event->opcode == PLAYER_AMP_AUDIO_EVENT_ACK) {
        if (!pending_opcode || event->argument[0] != pending_sequence ||
            event->argument[1] != pending_opcode || event->argument[3] != 0 ||
            event->argument[2] > PLAYER_AMP_AUDIO_FAULT)
            return break_protocol(PLAYER_AUDIO_CLIENT_PROTOCOL);
        if ((pending_opcode == PLAYER_AMP_AUDIO_CMD_STOP && event->argument[2] != PLAYER_AMP_AUDIO_CLOSED) ||
            (pending_opcode == PLAYER_AMP_AUDIO_CMD_PAUSE && event->argument[2] != PLAYER_AMP_AUDIO_PAUSED))
            return break_protocol(PLAYER_AUDIO_CLIENT_PROTOCOL);
        acknowledged_sequence = pending_sequence;
        pending_sequence = pending_opcode = 0;
        status.state = event->argument[2];
        return 0;
    }
    if (event->opcode == PLAYER_AMP_AUDIO_EVENT_ERROR) {
        if ((int32_t)event->argument[0] >= 0 || event->argument[2] != PLAYER_AMP_AUDIO_FAULT ||
            event->argument[3] > command_sequence || event->argument[1] > status.written_samples)
            return break_protocol(PLAYER_AUDIO_CLIENT_PROTOCOL);
        int error = (int32_t)event->argument[0];
        /* An asynchronous failure can be queued just before STOP is sent.
         * It reports the previous command sequence and must not cancel the
         * newer STOP whose ACK is the proof of actual hardware silence.
         */
        if (pending_opcode == PLAYER_AMP_AUDIO_CMD_STOP && event->argument[3] < pending_sequence) {
            (void)fail_client(error);
            return 0;
        }
        command_error = error;
        pending_sequence = pending_opcode = 0;
        return fail_client(command_error);
    }
    if (event->opcode != PLAYER_AMP_AUDIO_EVENT_PROGRESS && event->opcode != PLAYER_AMP_AUDIO_EVENT_ENDED)
        return break_protocol(PLAYER_AUDIO_CLIENT_PROTOCOL);
    if (event->argument[0] < event_completed_samples || event->argument[0] > status.written_samples ||
        event->argument[2] > PLAYER_AMP_AUDIO_FAULT || event->argument[3] > 1u)
        return break_protocol(PLAYER_AUDIO_CLIENT_PROTOCOL);
    event_completed_samples = event->argument[0];
    if (status.completed_samples < event_completed_samples)
        status.completed_samples = event_completed_samples;
    status.underruns = event->argument[1];
    status.state = event->argument[2];
    status.clock_valid = event->argument[3];
    if (event->opcode == PLAYER_AMP_AUDIO_EVENT_ENDED &&
        (status.completed_samples != status.total_samples || status.state != PLAYER_AMP_AUDIO_DRAINED))
        return break_protocol(PLAYER_AUDIO_CLIENT_PROTOCOL);
    return 0;
}

static int command(uint32_t opcode, uint32_t a, uint32_t b)
{
    if (protocol_broken) return status.last_error;
    if (pending_opcode) return PLAYER_AUDIO_CLIENT_BUSY;
    (void)player_audio_client_service();
    if (protocol_broken) return status.last_error;
    if (status.last_error && opcode != PLAYER_AMP_AUDIO_CMD_STOP) return status.last_error;
    if (!remote_available()) return break_protocol(PLAYER_AUDIO_CLIENT_UNAVAILABLE);
    if (++command_sequence == 0) ++command_sequence;
    struct player_amp_message request = {
        .session = status.media_session_id, .sequence = command_sequence,
        .opcode = opcode, .argument = {a, b, 0, 0}
    };
    pending_sequence = command_sequence;
    pending_opcode = opcode;
    command_error = 0;
    if (player_amp_send(&bus->audio_host_to_remote, &request) != 1)
        return break_protocol(PLAYER_AUDIO_CLIENT_PROTOCOL);
    uint64_t deadline = platform_time_us() + COMMAND_TIMEOUT_US;
    while (pending_opcode && !protocol_broken && platform_time_us() < deadline) {
        (void)player_audio_client_service();
        if (pending_opcode) vTaskDelay(pdMS_TO_TICKS(5));
    }
    if (pending_opcode) return break_protocol(PLAYER_AUDIO_CLIENT_TIMEOUT);
    if (protocol_broken) return status.last_error;
    if (command_error) return command_error;
    if (acknowledged_sequence != command_sequence)
        return break_protocol(PLAYER_AUDIO_CLIENT_PROTOCOL);
    return 0;
}
#else
static int fail_client(int error)
{
    /* Local-owner faults also try to establish silence without silently
     * beginning another session. A failed close still leaves storage pinned.
     */
    if (player_audio_close() != 0) {
        client_recovery_required = 1;
        player_subcore_force_silence();
    }
    status.state = PLAYER_AMP_AUDIO_FAULT;
    status.clock_valid = 0;
    status.last_error = error;
    return error;
}
#endif

int player_audio_client_service(void)
{
    if (!status.session_active) return 0;
#if PLAYER_AUDIO_OWNER == PLAYER_AUDIO_OWNER_RISCV
    if (protocol_broken) return status.last_error;
    player_amp_service();
    if (!remote_available()) return break_protocol(PLAYER_AUDIO_CLIENT_UNAVAILABLE);
    struct player_amp_message event;
    int result;
    while ((result = player_amp_receive(&bus->audio_remote_to_host, &event)) != 0) {
        if (result < 0) return break_protocol(PLAYER_AUDIO_CLIENT_PROTOCOL);
        if (consume_event(&event) != 0) return status.last_error;
    }
    if (refresh_remote_stats() != 0) return break_protocol(PLAYER_AUDIO_CLIENT_PROTOCOL);
    return status.last_error;
#else
    if (status.state == PLAYER_AMP_AUDIO_FAULT) return status.last_error;
    int result = player_audio_service();
    player_audio_stats local;
    player_audio_get_stats(&local);
    status.completed_samples = local.completed_samples;
    status.underruns = local.queue_underruns + local.fifo_underruns;
    status.dma_errors = local.dma_errors;
    status.dma_timeouts = local.dma_timeouts;
    status.queue_underruns = local.queue_underruns;
    status.fifo_underruns = local.fifo_underruns;
    status.state = (uint32_t)local.state;
    if (result || local.state == PLAYER_AUDIO_FAULT)
        return fail_client(result ? result : local.last_error);
    return 0;
#endif
}

int player_audio_client_start(uint32_t total_samples, unsigned volume_percent)
{
    if (!total_samples || total_samples > UINT32_MAX / 2u || volume_percent > 100u)
        return PLAYER_AUDIO_CLIENT_ARGUMENT;
    if (status.session_active) return PLAYER_AUDIO_CLIENT_BUSY;
#if PLAYER_AUDIO_OWNER == PLAYER_AUDIO_OWNER_RISCV
    bus = (volatile struct player_amp_shared *)player_amp_state();
    if (!bus || !player_amp_header_valid(bus) || bus->audio_owner != PLAYER_AUDIO_OWNER_RISCV ||
        bus->host_state != PLAYER_AMP_RUNNING || bus->remote_state != PLAYER_AMP_RUNNING)
        return PLAYER_AUDIO_CLIENT_UNAVAILABLE;
    boot_session = bus->session;
    if (bus->audio_host_to_remote.write_pos != bus->audio_host_to_remote.read_pos ||
        bus->audio_remote_to_host.write_pos != bus->audio_remote_to_host.read_pos)
        return PLAYER_AUDIO_CLIENT_PROTOCOL;
    if (bus->audio_session_active) return PLAYER_AUDIO_CLIENT_BUSY;
    /* Previous STOP must have ACKed before this owner may reset PCM counters. */
    bus->pcm_write_pos = bus->pcm_read_pos = 0;
    player_amp_barrier();
    command_sequence = event_sequence = event_completed_samples = 0;
    pending_sequence = pending_opcode = acknowledged_sequence = 0;
    protocol_broken = command_error = 0;
#endif
    memset(&status, 0, sizeof(status));
    if (++next_media_session == 0) ++next_media_session;
    status.media_session_id = next_media_session;
    status.total_samples = total_samples;
    status.volume_percent = volume_percent;
    status.state = PLAYER_AMP_AUDIO_BUFFERING;
    status.session_active = 1;
#if PLAYER_AUDIO_OWNER == PLAYER_AUDIO_OWNER_RISCV
    return command(PLAYER_AMP_AUDIO_CMD_START, total_samples, volume_percent);
#else
    _Static_assert((int)PLAYER_AUDIO_FAULT == (int)PLAYER_AMP_AUDIO_FAULT, "Audio status ABI");
    int result = player_audio_set_volume(volume_percent);
    if (!result) result = player_audio_open();
    if (!result) result = player_audio_begin();
    if (result) return fail_client(result);
    status.clock_valid = 1;
    return 0;
#endif
}

unsigned player_audio_client_write_pcm(const int16_t *samples, unsigned count)
{
    if ((!samples && count) || !status.session_active || status.last_error ||
        status.written_samples == status.total_samples) return 0;
    if (player_audio_client_service() != 0) return 0;
    uint32_t remaining = status.total_samples - status.written_samples;
    if (count > remaining) count = remaining;
    unsigned accepted;
#if PLAYER_AUDIO_OWNER == PLAYER_AUDIO_OWNER_RISCV
    int result = player_amp_pcm_write(bus,
        (volatile int16_t *)(uintptr_t)(PLAYER_SHARED_UNCACHED + PLAYER_PCM_OFFSET), samples, count);
    if (result < 0) { (void)break_protocol(PLAYER_AUDIO_CLIENT_PROTOCOL); return 0; }
    accepted = (unsigned)result;
#else
    accepted = player_audio_write(samples, count);
#endif
    status.written_samples += accepted;
    return accepted;
}

int player_audio_client_mark_eof(void)
{
    if (!status.session_active || status.last_error || status.written_samples != status.total_samples)
        return PLAYER_AUDIO_CLIENT_STATE;
#if PLAYER_AUDIO_OWNER == PLAYER_AUDIO_OWNER_RISCV
    return command(PLAYER_AMP_AUDIO_CMD_EOF, status.written_samples, 0);
#else
    int result = player_audio_finish();
    return result ? fail_client(result) : 0;
#endif
}

int player_audio_client_pause(void)
{
    if (!status.session_active || status.last_error) return PLAYER_AUDIO_CLIENT_STATE;
#if PLAYER_AUDIO_OWNER == PLAYER_AUDIO_OWNER_RISCV
    return command(PLAYER_AMP_AUDIO_CMD_PAUSE, 0, 0);
#else
    int result = player_audio_pause();
    if (result) return fail_client(result);
    uint64_t deadline = platform_time_us() + COMMAND_TIMEOUT_US;
    while (platform_time_us() < deadline) {
        if (player_audio_client_service() != 0) return status.last_error;
        if (status.state == PLAYER_AMP_AUDIO_PAUSED) return 0;
        vTaskDelay(pdMS_TO_TICKS(5));
    }
    return fail_client(PLAYER_AUDIO_CLIENT_TIMEOUT);
#endif
}

int player_audio_client_resume(void)
{
    if (!status.session_active || status.last_error) return PLAYER_AUDIO_CLIENT_STATE;
#if PLAYER_AUDIO_OWNER == PLAYER_AUDIO_OWNER_RISCV
    return command(PLAYER_AMP_AUDIO_CMD_RESUME, 0, 0);
#else
    int result = player_audio_resume();
    return result ? fail_client(result) : 0;
#endif
}

int player_audio_client_set_volume(unsigned percent)
{
    if (percent > 100u) return PLAYER_AUDIO_CLIENT_ARGUMENT;
    if (!status.session_active || status.last_error) return PLAYER_AUDIO_CLIENT_STATE;
#if PLAYER_AUDIO_OWNER == PLAYER_AUDIO_OWNER_RISCV
    int result = command(PLAYER_AMP_AUDIO_CMD_SET_VOLUME, percent, 0);
#else
    int result = player_audio_set_volume(percent);
    if (result) (void)fail_client(result);
#endif
    if (!result) status.volume_percent = percent;
    return result;
}

void player_audio_client_get_status(player_audio_client_stats *output)
{
    if (!output) return;
    (void)player_audio_client_service();
#if PLAYER_AUDIO_OWNER == PLAYER_AUDIO_OWNER_RISCV
    /* Preserve hardware counters even after an ERROR/emergency reset. */
    (void)refresh_remote_stats();
#else
    player_audio_stats local;
    player_audio_get_stats(&local);
    status.completed_samples = local.completed_samples;
    status.dma_errors = local.dma_errors;
    status.dma_timeouts = local.dma_timeouts;
    status.queue_underruns = local.queue_underruns;
    status.fifo_underruns = local.fifo_underruns;
    status.underruns = local.queue_underruns + local.fifo_underruns;
#endif
    *output = status;
}

int player_audio_client_stop(void)
{
    if (!status.session_active) return 0;
    if (client_recovery_required) return status.last_error;
#if PLAYER_AUDIO_OWNER == PLAYER_AUDIO_OWNER_RISCV
    int result = command(PLAYER_AMP_AUDIO_CMD_STOP, 0, 0);
    if (result) return result;
    /* The ACK follows mute, DMA stop and close. PCM ownership is now released. */
    bus->pcm_write_pos = bus->pcm_read_pos = 0;
    player_amp_barrier();
#else
    player_audio_stats local;
    player_audio_get_stats(&local);
    status.completed_samples = local.completed_samples;
    int result = player_audio_close();
    if (result) return fail_client(result);
#endif
    status.state = PLAYER_AMP_AUDIO_CLOSED;
    status.clock_valid = 0;
    status.session_active = 0;
    status.last_error = 0;
    return 0;
}

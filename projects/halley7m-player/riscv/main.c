#include "player_amp.h"
#include "FreeRTOS.h"
#include "task.h"
#include <x2600.h>
#if PLAYER_AUDIO_OWNER == PLAYER_AUDIO_OWNER_RISCV
#include "../audio/player_audio.h"
#endif

static volatile struct player_amp_shared *const shared =
    (volatile struct player_amp_shared *)(uintptr_t)PLAYER_SHARED_PHYS;
static uint32_t session;

#if PLAYER_AUDIO_OWNER == PLAYER_AUDIO_OWNER_RISCV
/* @PRODUCTION_AUDIO_REMOTE_BEGIN@ */
static uint32_t rv_audio_session, rv_audio_command_sequence, rv_audio_event_sequence;
static uint32_t rv_audio_total, rv_audio_copied, rv_audio_pause_sequence;
static TickType_t rv_audio_progress_at;
static int rv_audio_live, rv_audio_eof, rv_audio_finished, rv_audio_ended;
static int rv_audio_failed, rv_audio_error_pending;

static int rv_audio_event(uint32_t opcode, uint32_t a, uint32_t b, uint32_t c, uint32_t d)
{
    uint32_t sequence = rv_audio_event_sequence + 1u;
    if (!sequence) ++sequence;
    struct player_amp_message event = {
        .session = rv_audio_session, .sequence = sequence, .opcode = opcode,
        .argument = {a, b, c, d}
    };
    int result = player_amp_send(&shared->audio_remote_to_host, &event);
    if (result == 1) rv_audio_event_sequence = sequence;
    return result;
}

static void rv_audio_publish(void)
{
    player_audio_stats current;
    player_audio_get_stats(&current);
    shared->audio_media_session_id = rv_audio_session;
    shared->audio_total_samples = rv_audio_total;
    shared->audio_completed_frames = current.completed_samples;
    shared->audio_underruns = current.queue_underruns + current.fifo_underruns;
    shared->audio_dma_errors = current.dma_errors;
    shared->audio_dma_timeouts = current.dma_timeouts;
    shared->audio_queue_underruns = current.queue_underruns;
    shared->audio_fifo_underruns = current.fifo_underruns;
    shared->audio_state = rv_audio_failed ? PLAYER_AMP_AUDIO_FAULT : (uint32_t)current.state;
    shared->audio_clock_valid = rv_audio_live && !rv_audio_failed &&
                                current.state != PLAYER_AUDIO_CLOSED && current.state != PLAYER_AUDIO_FAULT;
    shared->audio_session_active = (uint32_t)rv_audio_live;
    player_amp_barrier();
}

static void rv_audio_fail(int error)
{
    if (!rv_audio_failed) {
        /* Mute and try to stop/release DMA before reporting an error. The
         * logical session remains pinned until an explicit successful STOP.
         */
        (void)player_audio_close();
        rv_audio_failed = 1;
        rv_audio_pause_sequence = 0;
    }
    shared->audio_error = error ? error : -10;
    rv_audio_error_pending = 1;
    rv_audio_publish();
}

static void rv_audio_ack(uint32_t sequence, uint32_t opcode)
{
    rv_audio_publish();
    if (rv_audio_event(PLAYER_AMP_AUDIO_EVENT_ACK, sequence, opcode, shared->audio_state, 0) != 1)
        rv_audio_fail(-10);
}

static void rv_audio_command(const struct player_amp_message *command)
{
    if (command->opcode == PLAYER_AMP_AUDIO_CMD_START && !rv_audio_live) {
        if (!command->session || command->session == rv_audio_session || command->sequence != 1)
            { rv_audio_fail(-10); return; }
        rv_audio_session = command->session;
        rv_audio_event_sequence = 0;
        rv_audio_command_sequence = 1;
        rv_audio_live = 1;
        rv_audio_failed = rv_audio_error_pending = 0;
        rv_audio_eof = rv_audio_finished = rv_audio_ended = 0;
        rv_audio_copied = rv_audio_pause_sequence = 0;
        rv_audio_progress_at = xTaskGetTickCount();
        rv_audio_total = command->argument[0];
        shared->audio_error = 0;
        if (!rv_audio_total || rv_audio_total > UINT32_MAX / 2u || command->argument[1] > 100u ||
            command->argument[2] || command->argument[3] ||
            shared->pcm_read_pos || shared->pcm_write_pos)
            { rv_audio_fail(-10); return; }
        int result = player_audio_set_volume(command->argument[1]);
        if (!result) result = player_audio_open();
        if (!result) result = player_audio_begin();
        if (result) { rv_audio_fail(result); return; }
        rv_audio_ack(command->sequence, command->opcode);
        return;
    }
    uint32_t expected = rv_audio_command_sequence + 1u;
    if (!expected) ++expected;
    if (!rv_audio_live || command->session != rv_audio_session || command->sequence != expected ||
        command->argument[2] || command->argument[3] || rv_audio_pause_sequence)
        { rv_audio_fail(-10); return; }
    rv_audio_command_sequence = expected;
    if (rv_audio_failed && command->opcode != PLAYER_AMP_AUDIO_CMD_STOP) return;
    int result;
    if (command->opcode == PLAYER_AMP_AUDIO_CMD_STOP && !command->argument[0] && !command->argument[1]) {
        result = player_audio_close();
        if (result) { rv_audio_fail(result); return; }
        rv_audio_live = rv_audio_failed = rv_audio_error_pending = 0;
        shared->audio_error = 0;
        rv_audio_ack(command->sequence, command->opcode);
    } else if (command->opcode == PLAYER_AMP_AUDIO_CMD_PAUSE && !command->argument[0] && !command->argument[1]) {
        result = player_audio_pause();
        if (result) { rv_audio_fail(result); return; }
        rv_audio_pause_sequence = command->sequence;
    } else if (command->opcode == PLAYER_AMP_AUDIO_CMD_RESUME && !command->argument[0] && !command->argument[1]) {
        result = player_audio_resume();
        if (result) { rv_audio_fail(result); return; }
        rv_audio_ack(command->sequence, command->opcode);
    } else if (command->opcode == PLAYER_AMP_AUDIO_CMD_SET_VOLUME && command->argument[0] <= 100u && !command->argument[1]) {
        result = player_audio_set_volume(command->argument[0]);
        if (result) { rv_audio_fail(result); return; }
        rv_audio_ack(command->sequence, command->opcode);
    } else if (command->opcode == PLAYER_AMP_AUDIO_CMD_EOF && command->argument[0] == rv_audio_total &&
               !command->argument[1] && shared->pcm_write_pos == rv_audio_total) {
        rv_audio_eof = 1;
        rv_audio_ack(command->sequence, command->opcode);
    } else {
        rv_audio_fail(-10);
    }
}

static void rv_audio_service(void)
{
    struct player_amp_message command;
    int result;
    while ((result = player_amp_receive(&shared->audio_host_to_remote, &command)) != 0) {
        if (result < 0) { rv_audio_fail(-10); break; }
        rv_audio_command(&command);
    }
    if (rv_audio_live && !rv_audio_failed) {
        result = player_audio_service();
        if (result) { rv_audio_fail(result); return; }
        uint32_t offset;
        int available = player_amp_pcm_peek(shared, &offset);
        if (available < 0 || shared->pcm_write_pos > rv_audio_total ||
            shared->pcm_read_pos != rv_audio_copied)
            { rv_audio_fail(-10); return; }
        if (available && !rv_audio_finished) {
            const int16_t *pcm = (const int16_t *)(uintptr_t)(PLAYER_SHARED_PHYS + offset);
            unsigned copied = player_audio_write(pcm, (unsigned)available);
            if (copied > (unsigned)available || player_amp_pcm_consume(shared, copied) != 0)
                { rv_audio_fail(-10); return; }
            rv_audio_copied += copied;
        }
        if (rv_audio_eof && !rv_audio_finished && rv_audio_copied == rv_audio_total) {
            result = player_audio_finish();
            if (result) { rv_audio_fail(result); return; }
            rv_audio_finished = 1;
        }
        rv_audio_publish();
        if (shared->audio_state == PLAYER_AMP_AUDIO_FAULT)
            { rv_audio_fail(-3); return; }
        if (rv_audio_pause_sequence && shared->audio_state == PLAYER_AMP_AUDIO_PAUSED) {
            uint32_t sequence = rv_audio_pause_sequence;
            rv_audio_pause_sequence = 0;
            rv_audio_ack(sequence, PLAYER_AMP_AUDIO_CMD_PAUSE);
        }
        if (!rv_audio_ended && shared->audio_state == PLAYER_AMP_AUDIO_DRAINED) {
            if (!rv_audio_finished || shared->audio_completed_frames != rv_audio_total)
                { rv_audio_fail(-10); return; }
            if (rv_audio_event(PLAYER_AMP_AUDIO_EVENT_ENDED, shared->audio_completed_frames,
                shared->audio_underruns, shared->audio_state, shared->audio_clock_valid) == 1)
                rv_audio_ended = 1;
        } else if (!rv_audio_ended &&
                   xTaskGetTickCount() - rv_audio_progress_at >= pdMS_TO_TICKS(100)) {
            if (rv_audio_event(PLAYER_AMP_AUDIO_EVENT_PROGRESS, shared->audio_completed_frames,
                shared->audio_underruns, shared->audio_state, shared->audio_clock_valid) == 1) {
                rv_audio_progress_at = xTaskGetTickCount();
            }
        }
    }
    if (rv_audio_error_pending && rv_audio_event(PLAYER_AMP_AUDIO_EVENT_ERROR,
        (uint32_t)shared->audio_error, shared->audio_completed_frames,
        PLAYER_AMP_AUDIO_FAULT, rv_audio_command_sequence) == 1)
        rv_audio_error_pending = 0;
}

static int rv_audio_halt(void)
{
    if (!rv_audio_live) return 0;
    int result = player_audio_close();
    if (result) { rv_audio_fail(result); return result; }
    rv_audio_live = 0;
    rv_audio_publish();
    return 0;
}
/* @PRODUCTION_AUDIO_REMOTE_END@ */
#else
static void rv_audio_service(void) { }
static int rv_audio_halt(void) { return 0; }
#endif

/* Returns -1 for an invalid range/contract, 0 for a matching CRC, and 1 for
 * different data. Validate offsets before constructing any DDR pointer.
 */
static int checksum_pcm(const struct player_amp_message *command, uint32_t *crc)
{
    uint32_t offset = command->argument[0];
    uint32_t length = command->argument[1];
    if (command->argument[3] != 0 || length == 0 || offset < PLAYER_PCM_OFFSET ||
        offset - PLAYER_PCM_OFFSET >= PLAYER_PCM_BYTES ||
        length > PLAYER_PCM_BYTES - (offset - PLAYER_PCM_OFFSET)) return -1;
    player_amp_barrier();
    const void *bytes = (const void *)(uintptr_t)(PLAYER_SHARED_PHYS + offset);
    *crc = player_amp_crc32(bytes, length);
    return *crc == command->argument[2] ? 0 : 1;
}

static void diagnostic_fault(uint32_t error)
{
    (void)rv_audio_halt();
    shared->remote_error = error;
    shared->remote_state = PLAYER_AMP_FAULT;
    shared->audio_clock_valid = 0;
    player_amp_barrier();
    /* Faults terminate this diagnostic session. Only a host-requested reset
     * and a fresh session may start another one; there is no auto-restart. */
    for (;;) vTaskSuspend(NULL);
}

static void send_event(uint32_t opcode, uint32_t sequence, uint32_t a, uint32_t b, uint32_t c)
{
    struct player_amp_message m = {
        .session = session, .sequence = sequence, .opcode = opcode,
        .argument = {a, b, c, 0}
    };
    if (player_amp_send(&shared->remote_to_host, &m) != 1) {
        diagnostic_fault(8);
    }
    RISC_CCU_Instance->MBOX_TO_HOST = opcode;
}

static void amp_task(void *unused)
{
    (void)unused;
#if PLAYER_AUDIO_OWNER == PLAYER_AUDIO_OWNER_RISCV
    _Static_assert((int)PLAYER_AUDIO_FAULT == (int)PLAYER_AMP_AUDIO_FAULT, "Audio state ABI");
#endif
    shared->remote_clock_hz = configCPU_CLOCK_HZ;
    shared->remote_tick_hz = configTICK_RATE_HZ;
    shared->remote_state = PLAYER_AMP_READY;
    player_amp_barrier();
    send_event(PLAYER_AMP_EVENT_READY, 0, PLAYER_AMP_VERSION,
               PLAYER_MEMORY_VERSION, shared->remote_build_crc32);
    uint32_t last_sequence = 0;
    TickType_t last_host_change = xTaskGetTickCount();
    TickType_t ready_since = last_host_change;
    uint32_t last_host_heartbeat = shared->host_heartbeat;
    for (;;) {
        if (!player_amp_header_valid(shared) || shared->session != session)
            diagnostic_fault(10);
        ++shared->remote_heartbeat;
        player_amp_barrier();
        RISC_CCU_Instance->MBOX_FROM_HOST = 0;
        rv_audio_service();
        struct player_amp_message command;
        int result;
        while ((result = player_amp_receive(&shared->host_to_remote, &command)) != 0) {
            uint32_t expected_sequence = last_sequence + 1u;
            if (expected_sequence == 0) ++expected_sequence;
            if (result < 0 || command.session != session || command.sequence != expected_sequence) {
                ++shared->remote_rejected_messages;
                diagnostic_fault(result < 0 ? 11 : 12);
            }
            last_sequence = command.sequence;
            if (command.opcode == PLAYER_AMP_CMD_ACK && shared->remote_state == PLAYER_AMP_READY) {
                shared->remote_state = PLAYER_AMP_RUNNING;
                send_event(PLAYER_AMP_EVENT_ACK, command.sequence, 0, 0, 0);
            } else if (command.opcode == PLAYER_AMP_CMD_PING && shared->remote_state == PLAYER_AMP_RUNNING) {
                send_event(PLAYER_AMP_EVENT_PONG, command.sequence, command.argument[0], 0, 0);
            } else if (command.opcode == PLAYER_AMP_CMD_CHECKSUM && shared->remote_state == PLAYER_AMP_RUNNING) {
                if (shared->audio_session_active) {
                    send_event(PLAYER_AMP_EVENT_ERROR, command.sequence, command.opcode, 17, 0);
                    diagnostic_fault(17);
                }
                uint32_t crc;
                int checksum_result = checksum_pcm(&command, &crc);
                if (checksum_result < 0) {
                    send_event(PLAYER_AMP_EVENT_ERROR, command.sequence, command.opcode, 15, 0);
                    diagnostic_fault(15);
                }
                send_event(PLAYER_AMP_EVENT_CHECKSUM, command.sequence,
                           command.argument[0], command.argument[1], crc);
                if (checksum_result > 0) diagnostic_fault(16);
            } else if (command.opcode == PLAYER_AMP_CMD_STOP &&
                       (shared->remote_state == PLAYER_AMP_READY || shared->remote_state == PLAYER_AMP_RUNNING)) {
                if (rv_audio_halt() != 0) diagnostic_fault(18);
                shared->remote_state = PLAYER_AMP_STOPPED;
                send_event(PLAYER_AMP_EVENT_STOPPED, command.sequence, 0, 0, 0);
                player_amp_barrier();
                /* This is a protocol diagnostic only; no audio DMA was started. */
                for (;;) vTaskSuspend(NULL);
            } else {
                send_event(PLAYER_AMP_EVENT_ERROR, command.sequence, command.opcode, 0, 0);
                diagnostic_fault(13);
            }
        }
        if (shared->remote_state == PLAYER_AMP_READY &&
            xTaskGetTickCount() - ready_since >= pdMS_TO_TICKS(1000))
            diagnostic_fault(14);
        if (shared->host_heartbeat != last_host_heartbeat) {
            last_host_heartbeat = shared->host_heartbeat;
            last_host_change = xTaskGetTickCount();
        } else if (xTaskGetTickCount() - last_host_change > pdMS_TO_TICKS(3000)) {
            diagnostic_fault(9);
        }
#if PLAYER_AUDIO_OWNER == PLAYER_AUDIO_OWNER_RISCV
        /* This SDK's priority-mask port cannot safely yield inside a critical
         * section: blocking NotifyTake would leave the tick IRQ masked after
         * switching tasks. Drain ISR notifications without blocking, then
         * yield via the already validated vTaskDelay path. ISR chains DMA.
         */
        if (rv_audio_live) {
            (void)ulTaskNotifyTake(pdTRUE, 0);
            vTaskDelay(pdMS_TO_TICKS(1));
        } else {
            vTaskDelay(pdMS_TO_TICKS(5));
        }
#else
        vTaskDelay(pdMS_TO_TICKS(5));
#endif
    }
}

int main(void)
{
    session = shared->session;
    /* The MIPS-owned comparison build must keep this core in reset. If a
     * loader accidentally releases it, never start its diagnostic task. */
    configASSERT(PLAYER_AUDIO_OWNER == PLAYER_AUDIO_OWNER_RISCV &&
                 shared->audio_owner == PLAYER_AUDIO_OWNER_RISCV);
    configASSERT(xTaskCreate(amp_task, "amp", 512, NULL, 3, NULL) == pdPASS);
    vTaskStartScheduler();
    player_riscv_assert(__FILE__, __LINE__);
}

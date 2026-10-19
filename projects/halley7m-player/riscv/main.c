#include "player_amp.h"
#include "FreeRTOS.h"
#include "task.h"
#include <x2600.h>

static volatile struct player_amp_shared *const shared =
    (volatile struct player_amp_shared *)(uintptr_t)PLAYER_SHARED_PHYS;
static uint32_t session;

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
        vTaskDelay(pdMS_TO_TICKS(5));
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

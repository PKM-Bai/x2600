#include "player_platform.h"
#include "player_embed.h"
#include "FreeRTOS.h"
#include "task.h"
#include <x2600_hal.h>
#include <stdio.h>

extern const unsigned char player_riscv_image_start[], player_riscv_image_end[];
static volatile struct player_amp_shared *const shared =
    (volatile struct player_amp_shared *)(uintptr_t)PLAYER_SHARED_UNCACHED;
static uint32_t next_session, next_sequence;
static uint64_t last_remote_change_us;
static uint32_t last_remote_heartbeat;
static int active;
static int shared_initialized;
static int recovery_required;
static uint32_t host_state = PLAYER_AMP_RESET;
static uint32_t pending_opcode, pending_sequence, pending_value, pending_length, pending_crc;
static uint32_t completed_sequence, completed_event;
static uint64_t command_deadline_us;
static const char *last_error = "none";

#define HOST_CFCR (*(volatile uint32_t *)UINT32_C(0xb2200fe0))
#define REMOTE_RESET UINT32_C(0x80000000)
#define PMA_ADDR(i) (*(volatile uint32_t *)(UINT32_C(0xb2a00040) + 4u * (i)))
#define PMA_CFG(i)  (*(volatile uint32_t *)(UINT32_C(0xb2a00060) + 4u * (i)))

static void hold_remote_reset(void)
{
    HOST_CFCR |= REMOTE_RESET;
    player_amp_barrier();
    HAL_uDelay(100);
    RISC_CCU_Instance->IRQ_MASK_L = 0;
    RISC_CCU_Instance->IRQ_MASK_H = 0;
    RISC_CCU_Instance->MBOX_FROM_HOST = 0;
    RISC_CCU_Instance->MBOX_TO_HOST = 0;
}

static void set_host_state(uint32_t state)
{
    host_state = state;
    if (shared_initialized) {
        shared->host_state = state;
        player_amp_barrier();
    }
}

static void fail_session(const char *reason)
{
    printf("AMP_FAIL reason=%s pending_opcode=%lu pending_seq=%lu\n", reason,
           (unsigned long)pending_opcode, (unsigned long)pending_sequence);
    last_error = reason;
    if (shared_initialized && shared->audio_session_active) {
        player_subcore_force_silence();
        last_error = reason;
        return;
    }
    hold_remote_reset();
    active = 0;
    pending_opcode = pending_sequence = 0;
    set_host_state(PLAYER_AMP_FAULT);
}

void player_subcore_force_silence(void)
{
    /* PE17 is the board amplifier enable. This single GPIO configuration
     * write does not touch PDMA, codec or shared CPM clock registers.
     */
    LL_GPIO_setPinMode(PE_Instance, 17, GPIO_MODE_OUTPUT0);
    player_amp_barrier();
    hold_remote_reset();
    LL_GPIO_setPinMode(PE_Instance, 17, GPIO_MODE_OUTPUT0);
    player_amp_barrier();
    recovery_required = 1;
    active = 0;
    pending_opcode = pending_sequence = 0;
    last_error = "audio-emergency-mute-board-reset-required";
    set_host_state(PLAYER_AMP_FAULT);
    printf("AMP_FORCE_SILENCE amplifier=PE17-low remote=reset buffers=pinned recovery=board-reset\n");
}

static int send_command(uint32_t opcode, uint32_t value, uint32_t length, uint32_t crc)
{
    if (pending_opcode) return -1;
    if (++next_sequence == 0) ++next_sequence;
    struct player_amp_message m = {
        .session = shared->session, .sequence = next_sequence,
        .opcode = opcode, .argument = { value, length, crc, 0 }
    };
    if (player_amp_send(&shared->host_to_remote, &m) != 1) {
        fail_session("command-ring");
        return -1;
    }
    pending_opcode = opcode;
    pending_sequence = m.sequence;
    pending_value = value;
    pending_length = length;
    pending_crc = crc;
    command_deadline_us = platform_time_us() + UINT64_C(500000);
    RISC_CCU_Instance->MBOX_FROM_HOST = m.sequence;
    return 0;
}

static void setup_remote(void)
{
    /* X2600 PM 4.4.3: PMA uses PMP-style addresses (physical >> 2),
     * C=bit5, A=bits4:3, X/W/R=bits2:0. Lower index wins.
     * Entry0: private 2 MiB cached RWX; entry1: shared 2 MiB uncached RW;
     * entry2: 0x10000000..0x1fffffff MMIO uncached RW. No cached DDR alias.
     * Configure only while CFCR.LEP_Reset holds the remote stopped.
     */
    for (unsigned i = 0; i < 8; ++i) PMA_CFG(i) = 0;
    PMA_ADDR(0) = (PLAYER_RISCV_PHYS >> 2) | ((PLAYER_RISCV_BYTES >> 3) - 1u);
    PMA_CFG(0) = 0x3f;
    PMA_ADDR(1) = (PLAYER_SHARED_PHYS >> 2) | ((PLAYER_SHARED_BYTES >> 3) - 1u);
    PMA_CFG(1) = 0x1b;
    PMA_ADDR(2) = 0x05ffffff;
    PMA_CFG(2) = 0x1b;
    RISC_CCU_Instance->CRER = PLAYER_RISCV_ENTRY;
    RISC_CCU_Instance->TIME_L = 0;
    RISC_CCU_Instance->TIME_H = 0;
    RISC_CCU_Instance->TIME_CMP_L = UINT32_MAX;
    RISC_CCU_Instance->TIME_CMP_H = UINT32_MAX;
    /* CCSR bit1 permits bus access, bit5 starts the machine timer. */
    RISC_CCU_Instance->CCSR = CCSR_IE | CCSR_BUS_MASK | CCSR_TIMER_EN;
    player_amp_barrier();
}

void player_amp_service(void)
{
    if (!active) return;
    if (!player_amp_header_valid(shared)) { fail_session("shared-header"); return; }
    ++shared->host_heartbeat;
    player_amp_barrier();
    RISC_CCU_Instance->MBOX_TO_HOST = 0;
    struct player_amp_message event;
    int result;
    while ((result = player_amp_receive(&shared->remote_to_host, &event)) != 0) {
        if (result < 0) { fail_session("event-ring-or-crc"); return; }
        if (event.session != shared->session) { fail_session("event-session"); return; }
        printf("AMP_EVENT session=%lu seq=%lu opcode=%lu value=%lu\n",
               (unsigned long)event.session, (unsigned long)event.sequence,
               (unsigned long)event.opcode, (unsigned long)event.argument[0]);
        if (event.opcode == PLAYER_AMP_EVENT_READY) {
            if (host_state != PLAYER_AMP_BOOTING || pending_opcode || event.sequence != 0 ||
                event.argument[0] != PLAYER_AMP_VERSION ||
                event.argument[1] != PLAYER_MEMORY_VERSION ||
                event.argument[2] != PLAYER_RISCV_IMAGE_CRC32) {
                fail_session("remote-ready-contract");
                return;
            }
            if (send_command(PLAYER_AMP_CMD_ACK, 0, 0, 0) != 0) return;
            continue;
        }
        uint32_t expected = pending_opcode == PLAYER_AMP_CMD_ACK ? PLAYER_AMP_EVENT_ACK :
                            pending_opcode == PLAYER_AMP_CMD_PING ? PLAYER_AMP_EVENT_PONG :
                            pending_opcode == PLAYER_AMP_CMD_STOP ? PLAYER_AMP_EVENT_STOPPED :
                            pending_opcode == PLAYER_AMP_CMD_CHECKSUM ? PLAYER_AMP_EVENT_CHECKSUM : 0;
        if (!expected || event.opcode != expected || event.sequence != pending_sequence ||
            (pending_opcode == PLAYER_AMP_CMD_PING && event.argument[0] != pending_value) ||
            (pending_opcode == PLAYER_AMP_CMD_CHECKSUM &&
             (event.argument[0] != pending_value || event.argument[1] != pending_length ||
              event.argument[2] != pending_crc || event.argument[3] != 0))) {
            fail_session("event-command-mismatch");
            return;
        }
        completed_sequence = event.sequence;
        completed_event = event.opcode;
        pending_opcode = pending_sequence = 0;
        if (event.opcode == PLAYER_AMP_EVENT_ACK) set_host_state(PLAYER_AMP_RUNNING);
        else if (event.opcode == PLAYER_AMP_EVENT_STOPPED) set_host_state(PLAYER_AMP_STOPPED);
    }
    if (shared->remote_state == PLAYER_AMP_FAULT) { fail_session("remote-fault"); return; }
    if (pending_opcode && platform_time_us() >= command_deadline_us) {
        fail_session("command-timeout");
        return;
    }
    uint32_t heartbeat = shared->remote_heartbeat;
    if (heartbeat != last_remote_heartbeat) {
        last_remote_heartbeat = heartbeat;
        last_remote_change_us = platform_time_us();
    } else if (platform_time_us() - last_remote_change_us > UINT64_C(3000000)) {
        fail_session("heartbeat-timeout");
    }
}

static int wait_command(uint32_t sequence, uint32_t expected_event)
{
    while (active && pending_sequence == sequence) {
        player_amp_service();
        if (pending_sequence == sequence) vTaskDelay(pdMS_TO_TICKS(5));
    }
    return active && completed_sequence == sequence && completed_event == expected_event ? 0 : -1;
}

static uint64_t remote_cycles(void)
{
    uint32_t high, low;
    do {
        high = RISC_CCU_Instance->TIME_H;
        low = RISC_CCU_Instance->TIME_L;
    } while (high != RISC_CCU_Instance->TIME_H);
    return ((uint64_t)high << 32) | low;
}

static int measure_remote_clock(void)
{
    uint64_t host_start = platform_time_us();
    uint64_t remote_start = remote_cycles();
    while (active && platform_time_us() - host_start < UINT64_C(100000)) {
        player_amp_service();
        if (active) vTaskDelay(pdMS_TO_TICKS(5));
    }
    if (!active) return -1;
    uint64_t delta = remote_cycles() - remote_start;
    uint64_t elapsed = platform_time_us() - host_start;
    uint64_t measured = elapsed && delta <= UINT64_MAX / UINT64_C(1000000) ?
                        delta * UINT64_C(1000000) / elapsed : 0;
    uint64_t configured = shared->remote_clock_hz;
    uint64_t difference = measured > configured ? measured - configured : configured - measured;
    printf("AMP_CLOCK measured_hz=%llu configured_hz=%llu elapsed_us=%llu cycles=%llu tolerance_percent=2 audio_clock_valid=0\n",
           (unsigned long long)measured, (unsigned long long)configured,
           (unsigned long long)elapsed, (unsigned long long)delta);
    if (!measured || !configured || difference * 100u > configured * 2u) {
        printf("AMP_CLOCK_ADJUST rebuild_riscv_clock_hz=%llu tick_calibrated=0\n",
               (unsigned long long)measured);
        fail_session("remote-timer-clock-mismatch");
        return -1;
    }
    return 0;
}

int player_subcore_start(void)
{
    if (recovery_required) {
        printf("AMP_RECOVERY_REQUIRED board-reset=1 buffers=pinned\n");
        return -1;
    }
    if (active || (shared_initialized && shared->audio_session_active)) {
        printf("AMP_BUSY stop-and-ack-before-next-session\n");
        return -1;
    }
    hold_remote_reset();
    active = 0;
    pending_opcode = pending_sequence = 0;
    completed_sequence = completed_event = 0;
    last_error = "none";
    /* The MIPS comparison mode must never release the remote reset, including
     * when an operator types "amp start". No audio service exists here yet. */
    if (PLAYER_AUDIO_OWNER == PLAYER_AUDIO_OWNER_MIPS) {
        set_host_state(PLAYER_AMP_RESET);
        printf("AMP_DISABLED owner=mips remote=reset audio_service=disabled\n");
        return 0;
    }
    size_t length = (size_t)(player_riscv_image_end - player_riscv_image_start);
    if (!length || length > PLAYER_RISCV_BYTES || length != PLAYER_RISCV_IMAGE_BYTES ||
        player_amp_crc32(player_riscv_image_start, length) != PLAYER_RISCV_IMAGE_CRC32) {
        fail_session("embedded-image");
        return -1;
    }
    /* U-Boot used high DDR before go. Evict cached aliases before using its
     * abandoned memory through KSEG1; no U-Boot service is called afterwards.
     */
    CleanInvalidateDCache_by_Addr((unsigned long *)(uintptr_t)(PLAYER_RISCV_PHYS | 0x80000000u),
                                PLAYER_RISCV_BYTES + PLAYER_SHARED_BYTES);
    volatile unsigned char *dst = (volatile unsigned char *)(uintptr_t)(PLAYER_RISCV_PHYS | 0xa0000000u);
    for (size_t i = 0; i < length; ++i) dst[i] = player_riscv_image_start[i];
    player_amp_barrier();
    if (player_amp_crc32((const void *)dst, length) != PLAYER_RISCV_IMAGE_CRC32) {
        fail_session("destination-crc");
        return -1;
    }
    if (++next_session == 0) ++next_session;
    next_sequence = 0;
    player_amp_initialize(shared, next_session, PLAYER_AUDIO_OWNER,
                          PLAYER_RISCV_IMAGE_CRC32, (uint32_t)length);
    shared_initialized = 1;
    set_host_state(PLAYER_AMP_BOOTING);
    setup_remote();
    printf("AMP_BOOT session=%lu image_bytes=%lu crc32=%08lx entry=%08lx owner=%s\n",
           (unsigned long)next_session, (unsigned long)length,
           (unsigned long)PLAYER_RISCV_IMAGE_CRC32, (unsigned long)PLAYER_RISCV_ENTRY,
           PLAYER_AUDIO_OWNER == PLAYER_AUDIO_OWNER_RISCV ? "riscv" : "mips");
    last_remote_heartbeat = 0;
    last_remote_change_us = platform_time_us();
    active = 1;
    HOST_CFCR &= ~REMOTE_RESET;
    player_amp_barrier();
    uint64_t deadline = platform_time_us() + UINT64_C(2000000);
    while (active && host_state != PLAYER_AMP_RUNNING && platform_time_us() < deadline) {
        player_amp_service();
        vTaskDelay(pdMS_TO_TICKS(5));
    }
    if (!active || host_state != PLAYER_AMP_RUNNING) {
        if (active) fail_session("ready-timeout");
        return -1;
    }
    if (measure_remote_clock() != 0) return -1;
    printf("AMP_READY session=%lu remote_clock=%lu remote_tick=%lu audio_clock_valid=0\n",
           (unsigned long)shared->session, (unsigned long)shared->remote_clock_hz,
           (unsigned long)shared->remote_tick_hz);
    return 0;
}

int player_subcore_stop(void)
{
    if (shared_initialized && shared->audio_session_active) {
        printf("AMP_BUSY stop-audio-and-confirm-silence-before-core-halt\n");
        return -1;
    }
    if (!active) {
        hold_remote_reset();
        printf("AMP_STOP already_reset=1 fault=%d audio_service=disabled\n", host_state == PLAYER_AMP_FAULT);
        return host_state == PLAYER_AMP_FAULT ? -1 : 0;
    }
    if (send_command(PLAYER_AMP_CMD_STOP, 0, 0, 0) != 0) return -1;
    uint32_t sequence = pending_sequence;
    int result = wait_command(sequence, PLAYER_AMP_EVENT_STOPPED);
    hold_remote_reset();
    active = 0;
    if (result == 0) set_host_state(PLAYER_AMP_STOPPED);
    printf("AMP_STOP seq=%lu acknowledged=%d audio_service=disabled\n",
           (unsigned long)sequence, result == 0);
    return result;
}

int player_subcore_ping(uint32_t value)
{
    if (!active || host_state != PLAYER_AMP_RUNNING) return -1;
    if (send_command(PLAYER_AMP_CMD_PING, value, 0, 0) != 0) return -1;
    return wait_command(pending_sequence, PLAYER_AMP_EVENT_PONG);
}

int player_subcore_shared_test(void)
{
    if (!active || host_state != PLAYER_AMP_RUNNING || pending_opcode || shared->audio_session_active) return -1;
    /* This deliberately exercises the reserved PCM DDR, without advancing
     * audio indices or enabling a clock/DMA service. Both mappings are uncached.
     */
    volatile unsigned char *pcm = (volatile unsigned char *)(uintptr_t)(PLAYER_SHARED_UNCACHED + PLAYER_PCM_OFFSET);
    for (uint32_t i = 0; i < PLAYER_PCM_BYTES; ++i)
        pcm[i] = (unsigned char)((i * 73u) ^ (i >> 8) ^ shared->session ^ 0xa5u);
    player_amp_barrier();
    uint32_t crc = player_amp_crc32((const void *)pcm, PLAYER_PCM_BYTES);
    if (send_command(PLAYER_AMP_CMD_CHECKSUM, PLAYER_PCM_OFFSET, PLAYER_PCM_BYTES, crc) != 0)
        return -1;
    uint32_t sequence = pending_sequence;
    int result = wait_command(sequence, PLAYER_AMP_EVENT_CHECKSUM);
    printf("AMP_SHARED seq=%lu offset=%08lx bytes=%lu crc32=%08lx acknowledged=%d audio_service=disabled\n",
           (unsigned long)sequence, (unsigned long)PLAYER_PCM_OFFSET,
           (unsigned long)PLAYER_PCM_BYTES, (unsigned long)crc, result == 0);
    return result;
}

const volatile struct player_amp_shared *player_amp_state(void) { return shared_initialized ? shared : NULL; }

void player_amp_print_status(void)
{
    printf("AMP_STATUS active=%d session=%lu host=%lu remote=%lu heartbeat=%lu error=%lu rejected=%lu owner=%s audio_clock_valid=%lu pending_opcode=%lu pending_seq=%lu last_error=%s audio_remote_active=%lu\n",
           active, (unsigned long)(shared_initialized ? shared->session : 0), (unsigned long)host_state,
           (unsigned long)(shared_initialized ? shared->remote_state : PLAYER_AMP_RESET),
           (unsigned long)(shared_initialized ? shared->remote_heartbeat : 0),
           (unsigned long)(shared_initialized ? shared->remote_error : 0),
           (unsigned long)(shared_initialized ? shared->remote_rejected_messages : 0),
           PLAYER_AUDIO_OWNER == PLAYER_AUDIO_OWNER_RISCV ? "riscv" : "mips",
           (unsigned long)(shared_initialized ? shared->audio_clock_valid : 0),
           (unsigned long)pending_opcode, (unsigned long)pending_sequence, last_error,
           (unsigned long)(shared_initialized ? shared->audio_session_active : 0));
}

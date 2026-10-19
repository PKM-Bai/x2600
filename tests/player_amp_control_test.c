/* The runner injects the current production amp_boot.c at the marker. Only
 * UART/time/cache/CCU/DDR boundaries are replaced; the state machine is real. */
#include "player_amp.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static uint64_t test_now;
static uint32_t test_cfcr, test_pma_address[8], test_pma_config[8];
static uint8_t test_remote_image[4];
static uint8_t test_pcm[PLAYER_PCM_BYTES];
static const uint8_t test_payload[4] = {1, 2, 3, 4};
static struct player_amp_shared test_shared;
static uint32_t test_gpio_port, test_gpio_pin, test_gpio_mode, test_gpio_calls;
#define PE_Instance (&test_gpio_port)
#define GPIO_MODE_OUTPUT0 8u
static void LL_GPIO_setPinMode(uint32_t *port, uint32_t pin, uint32_t mode)
{
    assert(port == PE_Instance);
    test_gpio_pin = pin; test_gpio_mode = mode; ++test_gpio_calls;
}
static struct {
    uint32_t CCSR, CRER, TIME_L, TIME_H, TIME_CMP_L, TIME_CMP_H;
    uint32_t MBOX_FROM_HOST, MBOX_TO_HOST, IRQ_MASK_L, IRQ_MASK_H;
} test_ccu;
#define RISC_CCU_Instance (&test_ccu)
#define CCSR_IE 1u
#define CCSR_BUS_MASK 2u
#define CCSR_TIMER_EN 32u
#define PLAYER_RISCV_IMAGE_BYTES 4u
#define PLAYER_RISCV_IMAGE_CRC32 (player_amp_crc32(test_payload, sizeof(test_payload)))
#define pdMS_TO_TICKS(ms) (ms)
static uint64_t platform_time_us(void) { return test_now; }
static void HAL_uDelay(uint32_t us);
static void vTaskDelay(uint32_t ticks);
void player_subcore_force_silence(void);
static void CleanInvalidateDCache_by_Addr(unsigned long *address, long bytes)
{ (void)address; (void)bytes; }

/* @PRODUCTION_AMP_BOOT@ */
/* @PRODUCTION_RISCV_CHECKSUM@ */

enum peer_mode { NORMAL, DROP_READY, DROP_ACK, DROP_PONG, WRONG_PONG_SEQUENCE,
                 BAD_PONG_CRC, BAD_EVENT_RING, DROP_STOP, WRONG_CLOCK,
                 DROP_CHECKSUM, WRONG_CHECKSUM_SEQUENCE, WRONG_CHECKSUM_OFFSET,
                 WRONG_CHECKSUM_LENGTH, WRONG_CHECKSUM_RESERVED, BAD_CHECKSUM_REPLY,
                 BAD_CHECKSUM_DATA };
static enum peer_mode mode;
static int remote_started;
static uint32_t peer_sequence;

static void HAL_uDelay(uint32_t us)
{
    test_now += us;
    if (test_cfcr & REMOTE_RESET) remote_started = 0;
}

static void peer_event(uint32_t opcode, uint32_t sequence, uint32_t value)
{
    struct player_amp_message e = {
        .session = test_shared.session, .sequence = sequence, .opcode = opcode,
        .argument = {value, 0, 0, 0}
    };
    if (opcode == PLAYER_AMP_EVENT_READY) {
        e.argument[0] = PLAYER_AMP_VERSION;
        e.argument[1] = PLAYER_MEMORY_VERSION;
        e.argument[2] = PLAYER_RISCV_IMAGE_CRC32;
    }
    assert(player_amp_send(&test_shared.remote_to_host, &e) == 1);
}

static void peer_checksum(const struct player_amp_message *command)
{
    assert(command->argument[0] == PLAYER_PCM_OFFSET);
    assert(command->argument[1] == PLAYER_PCM_BYTES && command->argument[3] == 0);
    for (uint32_t i = 0; i < PLAYER_PCM_BYTES; ++i)
        assert(test_pcm[i] == (uint8_t)((i * 73u) ^ (i >> 8) ^ test_shared.session ^ 0xa5u));
    if (mode == BAD_CHECKSUM_DATA) test_pcm[127] ^= 1u;
    uint32_t crc = 0;
    int result = checksum_pcm(command, &crc);
    assert(result == (mode == BAD_CHECKSUM_DATA ? 1 : 0));
    if (mode == DROP_CHECKSUM) return;
    struct player_amp_message event = {
        .session = test_shared.session,
        .sequence = command->sequence + (mode == WRONG_CHECKSUM_SEQUENCE),
        .opcode = PLAYER_AMP_EVENT_CHECKSUM,
        .argument = {command->argument[0] + (mode == WRONG_CHECKSUM_OFFSET),
                     command->argument[1] - (mode == WRONG_CHECKSUM_LENGTH),
                     crc ^ (mode == BAD_CHECKSUM_REPLY), mode == WRONG_CHECKSUM_RESERVED}
    };
    assert(player_amp_send(&test_shared.remote_to_host, &event) == 1);
    if (result > 0) test_shared.remote_state = PLAYER_AMP_FAULT;
}

static void vTaskDelay(uint32_t ticks)
{
    uint64_t us = (uint64_t)ticks * 1000u;
    test_now += us;
    if (test_cfcr & REMOTE_RESET) return;
    uint64_t clock = mode == WRONG_CLOCK ? UINT64_C(576000000) : UINT64_C(600000000);
    uint64_t cycles = ((uint64_t)test_ccu.TIME_H << 32) | test_ccu.TIME_L;
    cycles += us * clock / 1000000u;
    test_ccu.TIME_L = (uint32_t)cycles;
    test_ccu.TIME_H = (uint32_t)(cycles >> 32);
    ++test_shared.remote_heartbeat;
    if (!remote_started) {
        remote_started = 1;
        peer_sequence = 0;
        test_shared.remote_state = PLAYER_AMP_READY;
        test_shared.remote_clock_hz = 600000000;
        test_shared.remote_tick_hz = 1000;
        if (mode != DROP_READY) peer_event(PLAYER_AMP_EVENT_READY, 0, 0);
    }
    struct player_amp_message command;
    int result = player_amp_receive(&test_shared.host_to_remote, &command);
    if (!result) return;
    assert(result == 1 && command.session == test_shared.session);
    assert(command.sequence == ++peer_sequence);
    if (command.opcode == PLAYER_AMP_CMD_ACK) {
        if (mode != DROP_ACK) {
            test_shared.remote_state = PLAYER_AMP_RUNNING;
            peer_event(PLAYER_AMP_EVENT_ACK, command.sequence, 0);
        }
    } else if (command.opcode == PLAYER_AMP_CMD_PING) {
        if (mode == BAD_EVENT_RING) {
            test_shared.remote_to_host.write_pos = test_shared.remote_to_host.read_pos + PLAYER_AMP_RING_CAPACITY + 1u;
        } else if (mode != DROP_PONG) {
            peer_event(PLAYER_AMP_EVENT_PONG,
                       command.sequence + (mode == WRONG_PONG_SEQUENCE), command.argument[0]);
            if (mode == BAD_PONG_CRC)
                test_shared.remote_to_host.slots[test_shared.remote_to_host.read_pos % PLAYER_AMP_RING_CAPACITY].checksum ^= 1u;
        }
    } else if (command.opcode == PLAYER_AMP_CMD_CHECKSUM) {
        peer_checksum(&command);
    } else {
        assert(command.opcode == PLAYER_AMP_CMD_STOP);
        test_shared.remote_state = PLAYER_AMP_STOPPED;
        if (mode != DROP_STOP) peer_event(PLAYER_AMP_EVENT_STOPPED, command.sequence, 0);
    }
}

static void reset_fixture(enum peer_mode next_mode)
{
    test_now = 0;
    test_cfcr = 0;
    memset(&test_ccu, 0, sizeof(test_ccu));
    memset(&test_shared, 0, sizeof(test_shared));
    memset(test_pcm, 0, sizeof(test_pcm));
    active = shared_initialized = remote_started = 0;
    recovery_required = 0;
    test_gpio_pin = test_gpio_mode = test_gpio_calls = 0;
    next_session = next_sequence = pending_opcode = pending_sequence = 0;
    completed_sequence = completed_event = 0;
    host_state = PLAYER_AMP_RESET;
    mode = next_mode;
}

static void test_checksum_bounds(void)
{
    for (uint32_t i = 0; i < PLAYER_PCM_BYTES; ++i) test_pcm[i] = (uint8_t)(i ^ (i >> 8));
    struct player_amp_message command = {
        .opcode = PLAYER_AMP_CMD_CHECKSUM,
        .argument = {PLAYER_PCM_OFFSET, PLAYER_PCM_BYTES, 0, 0}
    };
    uint32_t expected = player_amp_crc32(test_pcm, sizeof(test_pcm));
    command.argument[2] = expected;
    uint32_t crc = 0;
    assert(checksum_pcm(&command, &crc) == 0 && crc == expected);
    command.argument[2] ^= 1u;
    assert(checksum_pcm(&command, &crc) == 1 && crc == expected);
    command.argument[0] = PLAYER_PCM_OFFSET + PLAYER_PCM_BYTES - 1u;
    command.argument[1] = 1;
    command.argument[2] = player_amp_crc32(test_pcm + PLAYER_PCM_BYTES - 1u, 1);
    assert(checksum_pcm(&command, &crc) == 0 && crc == command.argument[2]);
    const uint32_t invalid[][3] = {
        {0, 1, 0}, {PLAYER_PCM_OFFSET - 1u, 1, 0},
        {PLAYER_PCM_OFFSET, 0, 0}, {PLAYER_PCM_OFFSET, PLAYER_PCM_BYTES + 1u, 0},
        {PLAYER_PCM_OFFSET + PLAYER_PCM_BYTES, 1, 0},
        {PLAYER_PCM_OFFSET + PLAYER_PCM_BYTES - 1u, 2, 0},
        {PLAYER_PCM_OFFSET, UINT32_MAX, 0}, {UINT32_MAX, 1, 0},
        {PLAYER_RISCV_PHYS, 1, 0}, {PLAYER_SHARED_PHYS + PLAYER_PCM_OFFSET, 1, 0},
        {PLAYER_PCM_OFFSET, 1, 1}
    };
    for (unsigned i = 0; i < sizeof(invalid) / sizeof(invalid[0]); ++i) {
        command.argument[0] = invalid[i][0];
        command.argument[1] = invalid[i][1];
        command.argument[3] = invalid[i][2];
        crc = 0x11223344;
        assert(checksum_pcm(&command, &crc) == -1 && crc == 0x11223344);
    }
}

static void assert_terminal_fault(void)
{
    assert(!active && host_state == PLAYER_AMP_FAULT && (test_cfcr & REMOTE_RESET));
    assert(pending_opcode == 0 && pending_sequence == 0);
    player_amp_service();
    vTaskDelay(100);
    player_amp_service();
    assert(!active && host_state == PLAYER_AMP_FAULT && (test_cfcr & REMOTE_RESET));
}

int main(void)
{
    test_checksum_bounds();
    reset_fixture(NORMAL);
    if (PLAYER_AUDIO_OWNER == PLAYER_AUDIO_OWNER_MIPS) {
        assert(player_subcore_start() == 0 && (test_cfcr & REMOTE_RESET));
        assert(!active && !shared_initialized && player_amp_state() == NULL);
        assert(player_subcore_ping(1) == -1 && player_subcore_shared_test() == -1);
        assert(player_subcore_stop() == 0);
        printf("PASS AudioOwner=mips holds remote reset without shared-memory reads\n");
        return 0;
    }
    assert(player_subcore_start() == 0);
    struct player_amp_shared before_shared = test_shared;
    uint64_t before_busy = test_now;
    uint32_t before_cfcr = test_cfcr, before_session = next_session;
    assert(player_subcore_start() == -1 && active);
    assert(test_now == before_busy && test_cfcr == before_cfcr && next_session == before_session);
    assert(!memcmp(&test_shared, &before_shared, sizeof(test_shared)));
    test_shared.audio_session_active = 1;
    uint32_t before_audio_sequence = next_sequence;
    assert(player_subcore_shared_test() == -1 && player_subcore_stop() == -1);
    assert(next_sequence == before_audio_sequence && !(test_cfcr & REMOTE_RESET));
    active = 0; /* A failed core must not overwrite storage while DMA is pinned. */
    assert(player_subcore_start() == -1 && !(test_cfcr & REMOTE_RESET));
    active = 1;
    test_shared.audio_session_active = 0;
    assert(player_subcore_ping(0x12345678) == 0);
    assert(player_subcore_shared_test() == 0 && player_subcore_shared_test() == 0);
    assert(test_shared.pcm_write_pos == 0 && test_shared.pcm_read_pos == 0);
    assert(test_shared.audio_completed_frames == 0 && test_shared.audio_clock_valid == 0);
    assert(player_subcore_stop() == 0 && host_state == PLAYER_AMP_STOPPED);
    assert(player_subcore_shared_test() == -1);
    reset_fixture(DROP_READY);
    assert(player_subcore_start() == -1 && test_now < 2100000u);
    assert_terminal_fault();
    reset_fixture(DROP_ACK);
    assert(player_subcore_start() == -1 && !strcmp(last_error, "command-timeout"));
    assert_terminal_fault();
    const enum peer_mode broken[] = {DROP_PONG, WRONG_PONG_SEQUENCE, BAD_PONG_CRC, BAD_EVENT_RING};
    for (unsigned i = 0; i < sizeof(broken) / sizeof(broken[0]); ++i) {
        reset_fixture(NORMAL);
        assert(player_subcore_start() == 0);
        mode = broken[i];
        uint64_t before = test_now;
        assert(player_subcore_ping(99) == -1 && test_now - before < 510000u);
        assert_terminal_fault();
    }
    const enum peer_mode broken_checksum[] = {
        DROP_CHECKSUM, WRONG_CHECKSUM_SEQUENCE, WRONG_CHECKSUM_OFFSET, WRONG_CHECKSUM_LENGTH,
        WRONG_CHECKSUM_RESERVED, BAD_CHECKSUM_REPLY, BAD_CHECKSUM_DATA
    };
    for (unsigned i = 0; i < sizeof(broken_checksum) / sizeof(broken_checksum[0]); ++i) {
        reset_fixture(NORMAL);
        assert(player_subcore_start() == 0);
        mode = broken_checksum[i];
        uint64_t before = test_now;
        assert(player_subcore_shared_test() == -1 && test_now - before < 510000u);
        assert_terminal_fault();
    }
    reset_fixture(NORMAL);
    assert(player_subcore_start() == 0);
    mode = DROP_STOP;
    assert(player_subcore_stop() == -1 && !strcmp(last_error, "command-timeout"));
    assert_terminal_fault();
    reset_fixture(WRONG_CLOCK);
    assert(player_subcore_start() == -1 && !strcmp(last_error, "remote-timer-clock-mismatch"));
    assert_terminal_fault();
    reset_fixture(NORMAL);
    assert(player_subcore_start() == 0);
    test_shared.audio_session_active = 1;
    test_shared.pcm_read_pos = 13; test_shared.pcm_write_pos = 31;
    test_shared.magic ^= 1u;
    player_amp_service();
    assert(test_gpio_calls == 2 && test_gpio_pin == 17 && test_gpio_mode == GPIO_MODE_OUTPUT0);
    assert(recovery_required && test_shared.pcm_read_pos == 13 && test_shared.pcm_write_pos == 31);
    assert_terminal_fault();
    assert(player_subcore_start() == -1 && player_subcore_stop() == -1);
    printf("PASS production AMP commands: busy guard, ACK/PONG/STOP, 256KiB shared CRC/bounds/payload, bounded timeouts, terminal reset, timer mismatch\n");
    return 0;
}

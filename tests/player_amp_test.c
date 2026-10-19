#include "player_amp.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static struct player_amp_shared shared;
static int16_t pcm[PLAYER_AMP_PCM_SAMPLES];
static int16_t samples[PLAYER_AMP_PCM_SAMPLES + 1u];

static struct player_amp_message message(uint32_t sequence)
{
    struct player_amp_message m = {0};
    m.session = 7;
    m.sequence = sequence;
    m.opcode = PLAYER_AMP_CMD_PING;
    m.argument[0] = sequence * 13u;
    m.argument[1] = ~sequence;
    return m;
}

static void header_test(void)
{
    player_amp_initialize(&shared, 7, PLAYER_AUDIO_OWNER_RISCV, 0x1234, 4096);
    assert(player_amp_header_valid(&shared));
    shared.version++;
    assert(!player_amp_header_valid(&shared));
    shared.version--;
    shared.memory_version++;
    assert(!player_amp_header_valid(&shared));
    shared.memory_version--;
    shared.session = 0;
    assert(!player_amp_header_valid(&shared));
    shared.session = 7;
    shared.audio_owner = 99;
    assert(!player_amp_header_valid(&shared));
    shared.audio_owner = PLAYER_AUDIO_OWNER_MIPS;
    assert(player_amp_header_valid(&shared));
    shared.riscv_image_bytes = PLAYER_RISCV_BYTES + 1u;
    assert(!player_amp_header_valid(&shared));
    shared.riscv_image_bytes = 0;
    assert(!player_amp_header_valid(&shared));
}

static void rings_test(void)
{
    player_amp_initialize(&shared, 7, PLAYER_AUDIO_OWNER_RISCV, 0x1234, 4096);
    struct player_amp_message received;
    assert(player_amp_receive(&shared.host_to_remote, &received) == 0);
    for (uint32_t i = 0; i < PLAYER_AMP_RING_CAPACITY; ++i) {
        struct player_amp_message m = message(i);
        assert(player_amp_send(&shared.host_to_remote, &m) == 1);
    }
    struct player_amp_message full = message(42);
    assert(player_amp_send(&shared.host_to_remote, &full) == 0);
    for (uint32_t i = 0; i < PLAYER_AMP_RING_CAPACITY; ++i) {
        assert(player_amp_receive(&shared.host_to_remote, &received) == 1);
        assert(received.sequence == i && received.argument[0] == i * 13u);
        assert(received.argument[1] == ~i);
    }
    /* Exercise counter overflow independently from payload and slot wrap. */
    shared.host_to_remote.write_pos = shared.host_to_remote.read_pos = UINT32_MAX - 3u;
    for (uint32_t i = 0; i < 10000; ++i) {
        struct player_amp_message m = message(i);
        assert(player_amp_send(&shared.host_to_remote, &m) == 1);
        assert(player_amp_receive(&shared.host_to_remote, &received) == 1);
        assert(received.sequence == i && received.argument[0] == i * 13u);
    }
    struct player_amp_message damaged = message(99);
    assert(player_amp_send(&shared.host_to_remote, &damaged) == 1);
    shared.host_to_remote.slots[shared.host_to_remote.read_pos % PLAYER_AMP_RING_CAPACITY].argument[0] ^= 1u;
    assert(player_amp_receive(&shared.host_to_remote, &received) == -1);
    assert(player_amp_receive(&shared.host_to_remote, &received) == 0);
    shared.host_to_remote.write_pos = shared.host_to_remote.read_pos + PLAYER_AMP_RING_CAPACITY + 1u;
    assert(player_amp_receive(&shared.host_to_remote, &received) == -1);
    assert(player_amp_send(&shared.host_to_remote, &damaged) == -1);

    /* The reverse ring has independent producer/consumer positions. */
    damaged.opcode = PLAYER_AMP_EVENT_PONG;
    assert(player_amp_send(&shared.remote_to_host, &damaged) == 1);
    assert(player_amp_receive(&shared.remote_to_host, &received) == 1);
    assert(received.opcode == PLAYER_AMP_EVENT_PONG && received.session == 7);
    shared.remote_heartbeat = 123;
    shared.audio_completed_frames = 9600;
    shared.audio_clock_valid = 1;
    player_amp_initialize(&shared, 8, PLAYER_AUDIO_OWNER_MIPS, 0x5678, 2048);
    assert(shared.session == 8 && shared.remote_heartbeat == 0);
    assert(shared.audio_completed_frames == 0 && shared.audio_clock_valid == 0);
    assert(player_amp_receive(&shared.remote_to_host, &received) == 0);
    assert(player_amp_receive(&shared.host_to_remote, &received) == 0);
}

static void pcm_test(void)
{
    player_amp_initialize(&shared, 10, PLAYER_AUDIO_OWNER_RISCV, 1, 4096);
    for (uint32_t i = 0; i < PLAYER_AMP_PCM_SAMPLES + 1u; ++i)
        samples[i] = (int16_t)(i ^ (i >> 7));
    uint32_t offset = UINT32_MAX;
    assert(player_amp_pcm_peek(&shared, &offset) == 0 && offset == PLAYER_PCM_OFFSET);
    assert(player_amp_pcm_write(&shared, pcm, samples, PLAYER_AMP_PCM_SAMPLES + 1u) == PLAYER_AMP_PCM_SAMPLES);
    assert(player_amp_pcm_write(&shared, pcm, samples, 1) == 0);
    assert(player_amp_pcm_peek(&shared, &offset) == PLAYER_AMP_PCM_SAMPLES);
    assert(!memcmp(pcm, samples, sizeof(pcm)));
    assert(player_amp_pcm_consume(&shared, PLAYER_AMP_PCM_SAMPLES + 1u) == -1);
    assert(shared.pcm_read_pos == 0);
    assert(player_amp_pcm_consume(&shared, PLAYER_AMP_PCM_SAMPLES - 3u) == 0);
    assert(player_amp_pcm_write(&shared, pcm, samples, 7) == 7);
    assert(player_amp_pcm_peek(&shared, &offset) == 3);
    assert(offset == PLAYER_PCM_OFFSET + (PLAYER_AMP_PCM_SAMPLES - 3u) * 2u);
    assert(player_amp_pcm_consume(&shared, 3) == 0);
    assert(player_amp_pcm_peek(&shared, &offset) == 7 && offset == PLAYER_PCM_OFFSET);
    assert(!memcmp(pcm, samples, 7 * sizeof(int16_t)));
    assert(player_amp_pcm_consume(&shared, 7) == 0);
    /* Both logical uint32 and physical ring wrap, preserving sample order. */
    shared.pcm_write_pos = shared.pcm_read_pos = UINT32_MAX - 3u;
    assert(player_amp_pcm_write(&shared, pcm, samples, 17) == 17);
    uint32_t consumed = 0;
    while (consumed < 17) {
        int count = player_amp_pcm_peek(&shared, &offset);
        assert(count > 0 && offset >= PLAYER_PCM_OFFSET && offset < PLAYER_PCM_OFFSET + PLAYER_PCM_BYTES);
        assert(!memcmp(pcm + (offset - PLAYER_PCM_OFFSET) / 2u,
                       samples + consumed, (size_t)count * sizeof(int16_t)));
        assert(player_amp_pcm_consume(&shared, (uint32_t)count) == 0);
        consumed += (uint32_t)count;
    }
    assert(player_amp_pcm_peek(&shared, &offset) == 0);
    assert(shared.pcm_write_pos == 13 && shared.pcm_read_pos == 13);
    assert(player_amp_pcm_write(&shared, pcm, NULL, 1) == -1);
    assert(player_amp_pcm_peek(&shared, NULL) == -1);
    shared.pcm_write_pos = shared.pcm_read_pos + PLAYER_AMP_PCM_SAMPLES + 1u;
    assert(player_amp_pcm_write(&shared, pcm, samples, 1) == -1);
    assert(player_amp_pcm_peek(&shared, &offset) == -1);
    assert(player_amp_pcm_consume(&shared, 0) == -1);
}

int main(void)
{
    assert(player_amp_crc32("123456789", 9) == 0xcbf43926u);
    assert(player_amp_crc32("", 0) == 0);
    header_test();
    rings_test();
    pcm_test();
    printf("PASS AMP CRC/header/full-empty/order/10000 messages/counter wrap/corruption/reset/PCM-copy-release; control=%lu PCM=%u\n",
           (unsigned long)sizeof(shared), PLAYER_PCM_BYTES);
    return 0;
}

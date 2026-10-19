#include "player_session_core.h"
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint64_t input_position;
static unsigned reads, pumps, cancel_pump;
static uint32_t largest_read;
static const uint64_t input_size = UINT64_C(0xfffffff0);
static int source_seek(void *opaque, uint64_t absolute)
{ (void)opaque; if (absolute > input_size) return -EINVAL; input_position = absolute; return 0; }
static uint64_t source_size(void *opaque) { (void)opaque; return input_size; }
static int32_t source_read(void *opaque, void *output, uint32_t bytes)
{
    (void)opaque;
    if (bytes > input_size - input_position) bytes = (uint32_t)(input_size - input_position);
    if (bytes > largest_read) largest_read = bytes;
    ++reads;
    for (uint32_t i = 0; i < bytes; ++i) ((uint8_t *)output)[i] = (uint8_t)(input_position + i);
    input_position += bytes;
    return (int32_t)bytes;
}
static int pump(void *unused) { (void)unused; ++pumps; return cancel_pump ? -ECANCELED : 0; }

static void cache_test(void)
{
    struct player_media_io source = {NULL, source_read, source_seek, source_size};
    player_read_ahead cache;
    uint8_t backing[4096], output[7000];
    assert(player_read_ahead_init(&cache, &source, backing, sizeof(backing), pump, NULL) == 0);
    assert(cache.io.read(cache.io.opaque, output, sizeof(output)) == sizeof(output));
    for (unsigned i = 0; i < sizeof(output); ++i) assert(output[i] == (uint8_t)i);
    assert(largest_read <= PLAYER_READ_IO_BYTES && pumps == reads * 2u);
    unsigned before = reads;
    assert(cache.io.seek(cache.io.opaque, 6000u) == 0);
    assert(cache.io.read(cache.io.opaque, output, 300u) == 300);
    assert(reads == before);
    for (unsigned i = 0; i < 300u; ++i) assert(output[i] == (uint8_t)(6000u + i));
    assert(cache.io.seek(cache.io.opaque, input_size - 17u) == 0);
    assert(cache.io.read(cache.io.opaque, output, 100u) == 17);
    for (unsigned i = 0; i < 17u; ++i) assert(output[i] == (uint8_t)(input_size - 17u + i));
    assert(cache.io.read(cache.io.opaque, output, 1) == 0);
    assert(cache.io.seek(cache.io.opaque, UINT64_C(0x100000000)) < 0);
    assert(cache.io.seek(cache.io.opaque, 0) == 0);
    cancel_pump = 1;
    assert(cache.io.read(cache.io.opaque, output, 100) == -ECANCELED);
    before = reads;
    assert(player_read_ahead_step(&cache) == -ECANCELED && reads == before);
    cancel_pump = 0;
}

static void pool_test(void)
{
    _Alignas(128) uint8_t bytes[4096];
    player_packet_pool pool;
    player_packet_allocation a, b, c;
    assert(player_packet_pool_init(&pool, bytes, sizeof(bytes)) == 0);
    assert(player_packet_pool_reserve(&pool, 1000, 64, &a) == 0);
    assert(a.capacity >= 1064 && ((uintptr_t)a.data & 127u) == 0);
    memset(a.data, 0x11, 1000);
    memset(a.data + 1000, 0, 64);
    assert(player_packet_pool_reserve(&pool, 1500, 64, &b) == 0);
    memset(b.data, 0x22, 1500);
    assert(player_packet_pool_release(&pool, b.ticket) == 0);
    assert(player_packet_pool_used(&pool) == a.capacity + b.capacity);
    assert(player_packet_pool_release(&pool, b.ticket) == -ESTALE);
    assert(player_packet_pool_reserve(&pool, 2000, 64, &c) == -EAGAIN);
    assert(player_packet_pool_release(&pool, a.ticket) == 0);
    assert(player_packet_pool_used(&pool) == 0);
    assert(player_packet_pool_reserve(&pool, 4000, 64, &c) == 0);
    assert(c.capacity == 4096);
    assert(player_packet_pool_release(&pool, a.ticket) == -ESTALE);
    assert(player_packet_pool_release(&pool, c.ticket) == 0);
    for (unsigned repeat = 0; repeat < 3000u; ++repeat) {
        assert(player_packet_pool_reserve(&pool, 1000, 64, &a) == 0);
        assert(player_packet_pool_reserve(&pool, 1000, 64, &b) == 0);
        assert(player_packet_pool_release(&pool, a.ticket) == 0);
        assert(player_packet_pool_reserve(&pool, 1000, 64, &c) == 0);
        memset(c.data, 0x33, 1000);
        assert(player_packet_pool_release(&pool, c.ticket) == 0);
        assert(player_packet_pool_release(&pool, b.ticket) == 0);
        assert(!player_packet_pool_used(&pool));
    }
    /* Unsigned byte positions cross 4GiB without treating occupied bytes as
     * free; file offsets and streaming byte counters are separate domains. */
    pool.read_bytes = pool.write_bytes = UINT32_C(0xfffff000);
    assert(player_packet_pool_reserve(&pool, 2000, 64, &a) == 0);
    assert(player_packet_pool_reserve(&pool, 1000, 64, &b) == 0);
    assert(player_packet_pool_release(&pool, a.ticket) == 0);
    assert(player_packet_pool_reserve(&pool, 1500, 64, &c) == 0);
    assert(pool.write_bytes < pool.read_bytes && player_packet_pool_used(&pool) == 3584u);
    assert(player_packet_pool_release(&pool, c.ticket) == 0);
    assert(player_packet_pool_release(&pool, b.ticket) == 0);
    assert(!player_packet_pool_used(&pool));
}

static void packet_queue_test(void)
{
    player_packet_queue queue;
    player_queued_packet slots[3], input;
    memset(&input, 0, sizeof(input));
    player_packet_queue_init(&queue, slots, 3u);
    assert(!player_packet_queue_front(&queue));
    for (unsigned i = 0; i < 3u; ++i) {
        input.media.index = i;
        assert(player_packet_queue_push(&queue, &input) == 0);
    }
    assert(player_packet_queue_push(&queue, &input) == -EAGAIN);
    for (unsigned i = 0; i < 300u; ++i) {
        assert(player_packet_queue_front(&queue)->media.index == i);
        player_packet_queue_pop(&queue);
        input.media.index = i + 3u;
        assert(player_packet_queue_push(&queue, &input) == 0);
    }
}

int main(void)
{
    cache_test();
    pool_test();
    packet_queue_test();
    assert(player_video_timing_decide(1, 90, 1599) == PLAYER_VIDEO_WAIT);
    assert(player_video_timing_decide(1, 90, 1600) == PLAYER_VIDEO_PRESENT);
    assert(player_video_timing_decide(1, 90, 3199) == PLAYER_VIDEO_PRESENT);
    assert(player_video_timing_decide(1, 90, 3200) == PLAYER_VIDEO_DROP);
    assert(player_video_timing_decide(89, 90, 144000) == PLAYER_VIDEO_PRESENT);
    assert(player_video_timing_decide(UINT64_C(1000000), UINT64_C(1000100), UINT32_C(1600000000)) == PLAYER_VIDEO_PRESENT);
    puts("session core: read-ahead/pool/queue/30fps rational clock passed");
    return 0;
}

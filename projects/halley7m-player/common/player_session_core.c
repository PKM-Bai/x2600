#include "player_session_core.h"
#include <errno.h>
#include <stddef.h>
#include <string.h>

static int power_of_two(uint32_t value)
{
    return value && !(value & (value - 1u));
}

static int cache_seek(void *opaque, uint64_t absolute)
{
    player_read_ahead *cache = opaque;
    if (absolute > cache->file_size)
        return -EINVAL;
    if (absolute < cache->start || absolute > cache->start + cache->length) {
        cache->start = absolute;
        cache->start_slot = cache->length = 0;
    }
    cache->position = absolute;
    return 0;
}

static uint64_t cache_size(void *opaque)
{
    return ((player_read_ahead *)opaque)->file_size;
}

int player_read_ahead_step(player_read_ahead *cache)
{
    uint64_t end = cache->start + cache->length;
    uint32_t slot, available, count;
    int32_t received;
    int result;
    if (cache->error)
        return cache->error;
    if (cache->pump && (result = cache->pump(cache->pump_opaque)) != 0)
        return cache->error = result;
    if (end >= cache->file_size)
        return 0;
    if (cache->length == cache->capacity) {
        uint64_t consumed = cache->position - cache->start;
        uint32_t evict = consumed > PLAYER_READ_IO_BYTES ? PLAYER_READ_IO_BYTES : (uint32_t)consumed;
        if (!evict)
            return 0;
        cache->start += evict;
        cache->start_slot = (cache->start_slot + evict) & (cache->capacity - 1u);
        cache->length -= evict;
    }
    available = cache->capacity - cache->length;
    slot = (cache->start_slot + cache->length) & (cache->capacity - 1u);
    count = cache->capacity - slot;
    if (count > available) count = available;
    if (count > PLAYER_READ_IO_BYTES) count = PLAYER_READ_IO_BYTES;
    if ((uint64_t)count > cache->file_size - end) count = (uint32_t)(cache->file_size - end);
    if (cache->source_position != end) {
        result = cache->source.seek(cache->source.opaque, end);
        if (result) return cache->error = result < 0 ? result : -EIO;
        cache->source_position = end;
    }
    received = cache->source.read(cache->source.opaque, cache->bytes + slot, count);
    if (received <= 0 || (uint32_t)received > count)
        return cache->error = received < 0 ? received : -EIO;
    cache->source_position += (uint32_t)received;
    cache->length += (uint32_t)received;
    if (cache->pump && (result = cache->pump(cache->pump_opaque)) != 0)
        return cache->error = result;
    return received;
}

static int32_t cache_read(void *opaque, void *buffer, uint32_t bytes)
{
    player_read_ahead *cache = opaque;
    uint32_t copied = 0;
    if ((!buffer && bytes) || bytes > INT32_MAX)
        return -EINVAL;
    while (copied < bytes && cache->position < cache->file_size) {
        uint32_t relative = (uint32_t)(cache->position - cache->start);
        uint32_t available = cache->length - relative;
        uint32_t slot, count;
        if (!available) {
            int result = player_read_ahead_step(cache);
            if (result <= 0) return result ? result : -EIO;
            continue;
        }
        slot = (cache->start_slot + relative) & (cache->capacity - 1u);
        count = cache->capacity - slot;
        if (count > available) count = available;
        if (count > bytes - copied) count = bytes - copied;
        memcpy((uint8_t *)buffer + copied, cache->bytes + slot, count);
        copied += count;
        cache->position += count;
    }
    return (int32_t)copied;
}

int player_read_ahead_init(player_read_ahead *cache, const struct player_media_io *source,
                            uint8_t *bytes, uint32_t capacity, player_read_pump_fn pump, void *opaque)
{
    if (!cache || !source || !source->read || !source->seek || !source->size ||
        !bytes || !power_of_two(capacity)) return -EINVAL;
    memset(cache, 0, sizeof(*cache));
    cache->source = *source;
    cache->file_size = source->size(source->opaque);
    cache->bytes = bytes;
    cache->capacity = capacity;
    cache->pump = pump;
    cache->pump_opaque = opaque;
    cache->source_position = UINT64_MAX;
    cache->io = (struct player_media_io){cache, cache_read, cache_seek, cache_size};
    return 0;
}

int player_packet_pool_init(player_packet_pool *pool, uint8_t *bytes, uint32_t capacity)
{
    if (!pool || !bytes || ((uintptr_t)bytes & 127u) || !power_of_two(capacity) || capacity < 128u)
        return -EINVAL;
    memset(pool, 0, sizeof(*pool));
    pool->bytes = bytes;
    pool->capacity = capacity;
    return 0;
}

uint32_t player_packet_pool_used(const player_packet_pool *pool)
{
    return pool->write_bytes - pool->read_bytes;
}

int player_packet_pool_reserve(player_packet_pool *pool, uint32_t length, uint32_t padding,
                                player_packet_allocation *allocation)
{
    uint32_t amount, slot, gap = 0, free_bytes, ticket;
    player_packet_record *record;
    if (!allocation || !length || padding > UINT32_MAX - length || length + padding > UINT32_MAX - 127u)
        return -EINVAL;
    amount = (length + padding + 127u) & ~127u;
    if (amount > pool->capacity) return -E2BIG;
    if (pool->write_record - pool->read_record == PLAYER_PACKET_RECORDS) return -EAGAIN;
    slot = pool->write_bytes & (pool->capacity - 1u);
    if (amount > pool->capacity - slot) gap = pool->capacity - slot;
    free_bytes = pool->capacity - player_packet_pool_used(pool);
    if (gap > free_bytes || amount > free_bytes - gap) return -EAGAIN;
    pool->write_bytes += gap;
    allocation->data = pool->bytes + (pool->write_bytes & (pool->capacity - 1u));
    allocation->capacity = amount;
    ticket = pool->write_record + 1u;
    allocation->ticket = ticket;
    pool->write_bytes += amount;
    record = &pool->records[pool->write_record & (PLAYER_PACKET_RECORDS - 1u)];
    record->ticket = ticket;
    record->end = pool->write_bytes;
    record->released = 0;
    ++pool->write_record;
    return 0;
}

int player_packet_pool_release(player_packet_pool *pool, uint32_t ticket)
{
    uint32_t ordinal = ticket - 1u;
    player_packet_record *record;
    if (ordinal - pool->read_record >= pool->write_record - pool->read_record)
        return -ESTALE;
    record = &pool->records[ordinal & (PLAYER_PACKET_RECORDS - 1u)];
    if (record->ticket != ticket || record->released) return -ESTALE;
    record->released = 1;
    while (pool->read_record != pool->write_record) {
        record = &pool->records[pool->read_record & (PLAYER_PACKET_RECORDS - 1u)];
        if (!record->released) break;
        pool->read_bytes = record->end;
        ++pool->read_record;
    }
    /* A wholly empty byte pool can drop the unused tail before the next
     * allocation, avoiding a false full result for a large wrapped AU. */
    if (pool->read_record == pool->write_record)
        pool->read_bytes = pool->write_bytes =
            (pool->write_bytes + pool->capacity - 1u) & ~(pool->capacity - 1u);
    return 0;
}

void player_packet_queue_init(player_packet_queue *queue, player_queued_packet *packets, unsigned capacity)
{
    queue->packets = packets;
    queue->capacity = capacity;
    queue->read = queue->count = 0;
}

int player_packet_queue_push(player_packet_queue *queue, const player_queued_packet *packet)
{
    if (queue->count == queue->capacity) return -EAGAIN;
    queue->packets[(queue->read + queue->count) % queue->capacity] = *packet;
    ++queue->count;
    return 0;
}

player_queued_packet *player_packet_queue_front(player_packet_queue *queue)
{
    return queue->count ? &queue->packets[queue->read] : NULL;
}

void player_packet_queue_pop(player_packet_queue *queue)
{
    if (queue->count) {
        queue->read = (queue->read + 1u) % queue->capacity;
        --queue->count;
    }
}

enum player_video_timing player_video_timing_decide(uint64_t frame, uint64_t total_frames,
                                                    uint32_t completed_samples)
{
    uint64_t due = frame * UINT64_C(48000);
    uint64_t clock = (uint64_t)completed_samples * 30u;
    if (clock < due) return PLAYER_VIDEO_WAIT;
    if (frame + 1u != total_frames && clock - due >= 48000u)
        return PLAYER_VIDEO_DROP;
    return PLAYER_VIDEO_PRESENT;
}

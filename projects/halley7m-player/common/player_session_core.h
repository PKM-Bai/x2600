#ifndef PLAYER_SESSION_CORE_H
#define PLAYER_SESSION_CORE_H

#include "player_media.h"
#include <stdint.h>

#define PLAYER_READ_AHEAD_BYTES (2u * 1024u * 1024u)
#define PLAYER_PACKET_POOL_BYTES (2u * 1024u * 1024u)
#define PLAYER_READ_IO_BYTES 1024u
#define PLAYER_PACKET_RECORDS 512u
#define PLAYER_VIDEO_QUEUE_PACKETS 64u
#define PLAYER_AUDIO_QUEUE_PACKETS 256u

typedef int (*player_read_pump_fn)(void *opaque);
typedef struct player_read_ahead {
    struct player_media_io source;
    struct player_media_io io;
    uint8_t *bytes;
    uint32_t capacity, start_slot, length;
    uint64_t start, position, source_position, file_size;
    player_read_pump_fn pump;
    void *pump_opaque;
    int error;
} player_read_ahead;

int player_read_ahead_init(player_read_ahead *read_ahead, const struct player_media_io *source,
                            uint8_t *bytes, uint32_t capacity, player_read_pump_fn pump, void *opaque);
/* Reads at most one 1 KiB backing-file block and pumps before/after it.
 * Returns bytes prefetched, 0 if full/end, or negative error. */
int player_read_ahead_step(player_read_ahead *read_ahead);

typedef struct player_packet_allocation {
    uint8_t *data;
    uint32_t capacity;
    uint32_t ticket;
} player_packet_allocation;
typedef struct player_packet_record {
    uint32_t ticket, end;
    unsigned released;
} player_packet_record;
typedef struct player_packet_pool {
    uint8_t *bytes;
    uint32_t capacity, read_bytes, write_bytes, read_record, write_record;
    player_packet_record records[PLAYER_PACKET_RECORDS];
} player_packet_pool;

int player_packet_pool_init(player_packet_pool *pool, uint8_t *bytes, uint32_t capacity);
/* Allocation is contiguous and 128-byte aligned; padding is separate from
 * actual packet length. Released out of order, reclaimed in allocation order. */
int player_packet_pool_reserve(player_packet_pool *pool, uint32_t length, uint32_t padding,
                                player_packet_allocation *allocation);
int player_packet_pool_release(player_packet_pool *pool, uint32_t ticket);
uint32_t player_packet_pool_used(const player_packet_pool *pool);

typedef struct player_queued_packet {
    struct player_media_packet media;
    player_packet_allocation allocation;
    uint32_t consumed_samples;
} player_queued_packet;
typedef struct player_packet_queue {
    player_queued_packet *packets;
    unsigned capacity, read, count;
} player_packet_queue;
void player_packet_queue_init(player_packet_queue *queue, player_queued_packet *packets, unsigned capacity);
int player_packet_queue_push(player_packet_queue *queue, const player_queued_packet *packet);
player_queued_packet *player_packet_queue_front(player_packet_queue *queue);
void player_packet_queue_pop(player_packet_queue *queue);

enum player_video_timing { PLAYER_VIDEO_WAIT, PLAYER_VIDEO_PRESENT, PLAYER_VIDEO_DROP };
/* Compare source presentation timestamps with the consumed 48kHz sample
 * clock. Last means the final presentation-order output, not the last AU. */
enum player_video_timing player_video_timing_decide(int64_t pts_us, int64_t duration_us,
                                                    int last, uint32_t completed_samples);
uint32_t player_video_lateness_us(int64_t pts_us, uint32_t completed_samples);

#endif

#ifndef PLAYER_AMP_H
#define PLAYER_AMP_H

#include "player_memory.h"
#include <stddef.h>
#include <stdint.h>

#define PLAYER_AMP_MAGIC UINT32_C(0x50414d50)
#define PLAYER_AMP_VERSION UINT32_C(1)
#define PLAYER_AMP_RING_CAPACITY 16u
#define PLAYER_AUDIO_OWNER_MIPS 1u
#define PLAYER_AUDIO_OWNER_RISCV 2u

enum player_amp_state {
    PLAYER_AMP_RESET, PLAYER_AMP_BOOTING, PLAYER_AMP_READY,
    PLAYER_AMP_RUNNING, PLAYER_AMP_STOPPED, PLAYER_AMP_FAULT
};
enum player_amp_opcode {
    PLAYER_AMP_CMD_ACK = 1, PLAYER_AMP_CMD_PING, PLAYER_AMP_CMD_STOP,
    PLAYER_AMP_CMD_CHECKSUM,
    PLAYER_AMP_EVENT_READY = 0x100, PLAYER_AMP_EVENT_ACK,
    PLAYER_AMP_EVENT_PONG, PLAYER_AMP_EVENT_STOPPED, PLAYER_AMP_EVENT_ERROR,
    PLAYER_AMP_EVENT_CHECKSUM
};

/* CHECKSUM is a bring-up diagnostic; it does not start an audio service.
 * Command arguments: shared-region offset, byte count, expected CRC32, 0.
 * Event arguments: echoed offset, echoed byte count, actual CRC32, 0.
 * Only nonempty ranges wholly within PLAYER_PCM_OFFSET/PLAYER_PCM_BYTES are
 * accepted. Neither command nor event contains a core-private pointer.
 */

/* ABI uses only fixed-width integers, including the session and sequence.
 * Exactly one producer and one consumer own each ring. Shared mappings must
 * be uncached on BOTH cores; barriers publish the payload before write_pos.
 */
struct player_amp_message {
    uint32_t session;
    uint32_t sequence;
    uint32_t opcode;
    uint32_t argument[4];
    uint32_t checksum;
};
struct player_amp_ring {
    _Alignas(64) uint32_t write_pos;
    _Alignas(64) uint32_t read_pos;
    _Alignas(64) struct player_amp_message slots[PLAYER_AMP_RING_CAPACITY];
};
struct player_amp_shared {
    uint32_t magic;
    uint32_t version;
    uint32_t memory_version;
    uint32_t struct_bytes;
    uint32_t session;
    uint32_t audio_owner;
    uint32_t riscv_image_crc32;
    uint32_t riscv_image_bytes;
    _Alignas(64) uint32_t host_state;
    uint32_t host_heartbeat;
    _Alignas(64) uint32_t remote_state;
    uint32_t remote_heartbeat;
    uint32_t remote_error;
    uint32_t remote_clock_hz;
    uint32_t remote_tick_hz;
    uint32_t remote_build_crc32;
    uint32_t remote_rejected_messages;
    /* Audio is disabled in bring-up. These counters become meaningful only
     * after the audio owner starts its actual DMA/codec pipeline. */
    _Alignas(64) uint32_t pcm_write_pos;
    _Alignas(64) uint32_t pcm_read_pos;
    uint32_t audio_completed_frames;
    uint32_t audio_underruns;
    uint32_t audio_clock_valid;
    _Alignas(64) struct player_amp_ring host_to_remote;
    _Alignas(64) struct player_amp_ring remote_to_host;
};

_Static_assert(sizeof(struct player_amp_message) == 32, "AMP message ABI");
_Static_assert(sizeof(struct player_amp_ring) == 640, "AMP ring ABI");
_Static_assert(sizeof(struct player_amp_shared) == 1600, "AMP control ABI");
_Static_assert(sizeof(struct player_amp_shared) <= PLAYER_PCM_OFFSET,
               "Control structures overlap the PCM ring");

void player_amp_barrier(void);
uint32_t player_amp_crc32(const void *bytes, size_t length);
void player_amp_initialize(volatile struct player_amp_shared *shared,
                           uint32_t session, uint32_t audio_owner,
                           uint32_t image_crc32, uint32_t image_bytes);
int player_amp_header_valid(const volatile struct player_amp_shared *shared);
/* 1: transferred; 0: full/empty; -1: invalid ring distance/checksum. */
int player_amp_send(volatile struct player_amp_ring *ring,
                    const struct player_amp_message *message);
int player_amp_receive(volatile struct player_amp_ring *ring,
                       struct player_amp_message *message);

#endif

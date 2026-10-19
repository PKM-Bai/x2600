#ifndef PLAYER_ROTATOR_H
#define PLAYER_ROTATOR_H
#include <stdbool.h>
#include <stdint.h>

/* X2600 PM ch12: CUSTOM ARGB input -> display port, never NV12 memory output. */
#define PLAYER_ROTATOR_SLOTS 3u
#define PLAYER_ROTATOR_PHYSICAL_BASE 0x13070000u
#define PLAYER_ROTATOR_FRAME_SIZE 0x04u
#define PLAYER_ROTATOR_GLOBAL 0x08u
#define PLAYER_ROTATOR_CONTROL 0x0cu
#define PLAYER_ROTATOR_STATUS 0x10u
#define PLAYER_ROTATOR_CLEAR 0x14u
#define PLAYER_ROTATOR_MASK 0x18u
#define PLAYER_ROTATOR_READ_SITE 0x20u
#define PLAYER_ROTATOR_BUFFER 0x2cu
#define PLAYER_ROTATOR_STOP (1u << 2)
#define PLAYER_ROTATOR_FLUSH (1u << 4)
#define PLAYER_ROTATOR_EOF (1u << 2)
#define PLAYER_ROTATOR_SOF (1u << 3)
#define PLAYER_ROTATOR_ACK (1u << 1)
#define PLAYER_ROTATOR_WORKING 1u
#define PLAYER_ROTATOR_IRQS (PLAYER_ROTATOR_EOF | PLAYER_ROTATOR_SOF | PLAYER_ROTATOR_ACK)

typedef struct { uint32_t width, height, stride, bytes, frame_size, global; } player_rotator_layout_t;
typedef struct { uint32_t epoch, sequence, slot; } player_rotator_token_t;
enum { PLAYER_ROT_WRITING=1, PLAYER_ROT_READY=2, PLAYER_ROT_QUEUED=4,
       PLAYER_ROT_READING=8, PLAYER_ROT_LAST=16, PLAYER_ROT_PRESENT=32 };
typedef struct { uint32_t physical, sequence, flags; } player_rotator_slot_t;
typedef struct {
    player_rotator_slot_t slots[PLAYER_ROTATOR_SLOTS];
    player_rotator_layout_t layout;
    uint32_t epoch;
    int queued, reading, last, latest_started;
} player_rotator_t;

/* CUSTOM uses 512-byte row blocks. This adapter supports whole 128-pixel
 * blocks and 8-row groups, including 1280x720, without inventing edge padding. */
int player_rotator_layout(uint32_t width, uint32_t height, player_rotator_layout_t *out);
int player_rotator_init(player_rotator_t *rot, uint32_t width, uint32_t height,
                        const uint32_t physical[PLAYER_ROTATOR_SLOTS]);
int player_rotator_acquire(player_rotator_t *rot, player_rotator_token_t *token);
int player_rotator_cancel_write(player_rotator_t *rot, player_rotator_token_t token);
int player_rotator_written(player_rotator_t *rot, player_rotator_token_t token);
int player_rotator_queue(player_rotator_t *rot, player_rotator_token_t token);
int player_rotator_sof(player_rotator_t *rot, uint32_t read_site);
int player_rotator_eof(player_rotator_t *rot);
/* A real DPU output EOD plus a matching observed ROT SOF is required. EOW,
 * FETCH_ADDR and EOF alone cannot acknowledge presentation to the caller. */
bool player_rotator_display_ready(const player_rotator_t *rot, player_rotator_token_t token);
int player_rotator_displayed(player_rotator_t *rot, player_rotator_token_t token);
int player_rotator_repeat(player_rotator_t *rot, player_rotator_token_t *token);
/* Only after producer stopped and ROT ACK/idle. Timeout must leave every hold. */
int player_rotator_reset(player_rotator_t *rot, bool hardware_idle);
#endif

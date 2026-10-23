#ifndef AI_AIR1103_H
#define AI_AIR1103_H

#include <stddef.h>
#include <stdint.h>

#define AI_AIR1103_BAUD 2000000u
#define AI_AIR1103_MIC_BYTES 512u
#define AI_AIR1103_PLAY_BYTES 320u
#define AI_AIR1103_MAX_PAYLOAD 512u
#define AI_AIR1103_FRAME_OVERHEAD 7u
#define AI_AIR1103_MAX_FRAME (AI_AIR1103_MAX_PAYLOAD + AI_AIR1103_FRAME_OVERHEAD)
#define AI_AIR1103_MIC 0x2080u
#define AI_AIR1103_PLAY 0x2081u
#define AI_AIR1103_IDENTIFY 0x0180u
#define AI_AIR1103_STOP_MIC 0x0201u
#define AI_AIR1103_VOLUME 0x0203u
#define AI_AIR1103_RESET 0x0205u

/* The callback is synchronous. Payload is borrowed until it returns.
 * Feed is task-context only and must not be called recursively. PCM byte order
 * is preserved; upstream endianness still needs verification on the real unit. */
typedef void (*ai_air1103_frame_fn)(void *context, uint16_t command,
                                  const uint8_t *payload, size_t length);

typedef struct {
    uint64_t received_bytes;
    uint32_t frames, checksum_errors, length_errors, discarded_bytes;
} ai_air1103_stats;

typedef struct {
    uint8_t bytes[AI_AIR1103_MAX_FRAME];
    size_t used;
    ai_air1103_stats stats;
    ai_air1103_frame_fn on_frame;
    void *context;
} ai_air1103_parser;

void ai_air1103_init(ai_air1103_parser *parser, ai_air1103_frame_fn callback,
                     void *context);
/* Returns 0 on valid arguments, -1 on invalid arguments. Noise and bad frames
 * are counted and skipped, preserving later complete frames in the same feed. */
int ai_air1103_feed(ai_air1103_parser *parser, const uint8_t *bytes, size_t length);
/* Length is payload only, big endian. SUM8 covers header through payload.
 * Returns frame length, or 0 for invalid command payload/capacity. No IO occurs.
 * PLAY must be exactly 320 bytes; caller schedules one frame every 10 ms.
 * STOP_MIC is explicitly NOT a playback flush/stop command. */
size_t ai_air1103_pack(uint16_t command, const uint8_t *payload, size_t length,
                       uint8_t *output, size_t capacity);

#endif

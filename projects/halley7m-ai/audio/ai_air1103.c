#include "ai_air1103.h"
#include <string.h>

static int valid_length(uint16_t command, size_t length)
{
    if (length > AI_AIR1103_MAX_PAYLOAD) return 0;
    switch (command) {
    case AI_AIR1103_MIC: return length == AI_AIR1103_MIC_BYTES;
    case AI_AIR1103_PLAY: return length == AI_AIR1103_PLAY_BYTES;
    case AI_AIR1103_STOP_MIC: return length == 0;
    case AI_AIR1103_VOLUME: case AI_AIR1103_RESET: return length == 1;
    default: return 1;
    }
}

void ai_air1103_init(ai_air1103_parser *p, ai_air1103_frame_fn callback,
                     void *context)
{
    if (!p) return;
    memset(p, 0, sizeof(*p));
    p->on_frame = callback;
    p->context = context;
}

static void discard(ai_air1103_parser *p, size_t count)
{
    p->stats.discarded_bytes += (uint32_t)count;
    p->used -= count;
    memmove(p->bytes, p->bytes + count, p->used);
}

static void parse(ai_air1103_parser *p)
{
    while (p->used) {
        if (p->bytes[0] != 0x55u) { discard(p, 1); continue; }
        if (p->used < 2) return;
        if (p->bytes[1] != 0xaau) { discard(p, 1); continue; }
        if (p->used < 4) return;
        size_t length = ((size_t)p->bytes[2] << 8) | p->bytes[3];
        if (length > AI_AIR1103_MAX_PAYLOAD) {
            ++p->stats.length_errors;
            discard(p, 1);
            continue;
        }
        if (p->used < 6) return;
        uint16_t command = (uint16_t)(((uint16_t)p->bytes[4] << 8) | p->bytes[5]);
        if (!valid_length(command, length)) {
            ++p->stats.length_errors;
            discard(p, 1);
            continue;
        }
        size_t total = length + AI_AIR1103_FRAME_OVERHEAD;
        if (p->used < total) return;
        uint8_t sum = 0;
        for (size_t i = 0; i + 1 < total; ++i) sum = (uint8_t)(sum + p->bytes[i]);
        if (sum != p->bytes[total - 1]) {
            ++p->stats.checksum_errors;
            discard(p, 1);
            continue;
        }
        ++p->stats.frames;
        if (p->on_frame) p->on_frame(p->context, command, p->bytes + 6, length);
        p->used -= total;
        memmove(p->bytes, p->bytes + total, p->used);
    }
}

int ai_air1103_feed(ai_air1103_parser *p, const uint8_t *bytes, size_t length)
{
    if (!p || (length && !bytes)) return -1;
    p->stats.received_bytes += length;
    for (size_t i = 0; i < length; ++i) {
        /* parse always consumes a full maximum-size frame, valid or invalid. */
        p->bytes[p->used++] = bytes[i];
        parse(p);
    }
    return 0;
}

size_t ai_air1103_pack(uint16_t command, const uint8_t *payload, size_t length,
                       uint8_t *output, size_t capacity)
{
    if (!output || (length && !payload) || !valid_length(command, length) ||
        capacity < length + AI_AIR1103_FRAME_OVERHEAD) return 0;
    if ((command == AI_AIR1103_VOLUME && payload[0] > 31) ||
        (command == AI_AIR1103_RESET && payload[0] != 0)) return 0;
    output[0] = 0x55; output[1] = 0xaa;
    output[2] = (uint8_t)(length >> 8); output[3] = (uint8_t)length;
    output[4] = (uint8_t)(command >> 8); output[5] = (uint8_t)command;
    if (length) memcpy(output + 6, payload, length);
    uint8_t sum = 0;
    for (size_t i = 0; i < length + 6; ++i) sum = (uint8_t)(sum + output[i]);
    output[length + 6] = sum;
    return length + AI_AIR1103_FRAME_OVERHEAD;
}

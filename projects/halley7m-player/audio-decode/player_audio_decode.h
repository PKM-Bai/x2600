#ifndef PLAYER_AUDIO_DECODE_H
#define PLAYER_AUDIO_DECODE_H

#include <stdint.h>

typedef struct player_audio_decoder player_audio_decoder;

enum player_audio_codec {
    PLAYER_AUDIO_CODEC_MP3 = 1,
    PLAYER_AUDIO_CODEC_AAC_ADTS = 2,
    PLAYER_AUDIO_CODEC_AAC_RAW = 3
};

typedef struct player_audio_decode_config {
    enum player_audio_codec codec;
    /* AAC_RAW requires the MP4 esds AudioSpecificConfig (AAC-LC only).
     * The decoder consumes this at create; caller retains no lifetime duty. */
    const uint8_t *audio_specific_config;
    uint32_t audio_specific_config_bytes;
} player_audio_decode_config;

typedef struct player_audio_decoded_frame {
    /* Interleaved source-rate PCM; valid until next decode or destroy. */
    const int16_t *pcm;
    uint32_t frames; /* Per-channel PCM frames, not interleaved sample count. */
    uint32_t channels;
    uint32_t sample_rate;
    uint32_t consumed_bytes;
    /* Native decoder priming not returned as PCM. FAAD2 hides its first
     * 1024-frame LC output; account for this before applying container trim
     * again. Zero for minimp3. */
    uint32_t decoder_skipped_frames;
} player_audio_decoded_frame;

/* One MIPS media owner, no IO/tasks/UART/audio hardware calls. Feed exactly
 * one complete MP3 frame or AAC access unit (ADTS includes its header).
 * 0=decoded/consumed, negative errno=error. frames=0 with consumed_bytes>0
 * is valid decoder priming, never fabricated PCM. AAC supports LC initially;
 * reject explicitly signaled SBR/PS/other ASC profiles, channels outside 1/2,
 * or rate above 48kHz. HE-AAC is outside scope: an ADTS implicit extension
 * may decode only its backward-compatible LC core with this LC-only build.
 * This layer does not apply container edit lists or encoder delay/padding;
 * its caller owns that source-sample trim before PCM conversion. */
int player_audio_decoder_create(const player_audio_decode_config *config,
                                 player_audio_decoder **decoder);
int player_audio_decoder_decode(player_audio_decoder *decoder, const uint8_t *packet,
                                 uint32_t bytes, player_audio_decoded_frame *frame);
void player_audio_decoder_destroy(player_audio_decoder *decoder);

#endif

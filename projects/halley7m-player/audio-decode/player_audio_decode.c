#include "player_audio_decode.h"
#include "minimp3.h"
#include "neaacdec.h"
#include <errno.h>
#include <limits.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

struct player_audio_decoder {
    enum player_audio_codec codec;
    union { mp3dec_t mp3; NeAACDecHandle aac; } backend;
    int16_t mp3_pcm[MINIMP3_MAX_SAMPLES_PER_FRAME];
    uint32_t initialized, calls, rate, channels;
};

static int format_valid(uint32_t rate, uint32_t channels)
{
    return rate >= 8000u && rate <= 48000u && (channels == 1u || channels == 2u);
}

int player_audio_decoder_create(const player_audio_decode_config *config,
                                 player_audio_decoder **output)
{
    if (output) *output = NULL;
    if (!config || !output || config->codec < PLAYER_AUDIO_CODEC_MP3 ||
        config->codec > PLAYER_AUDIO_CODEC_AAC_RAW) return -EINVAL;
    if (config->codec == PLAYER_AUDIO_CODEC_AAC_RAW &&
        (!config->audio_specific_config || config->audio_specific_config_bytes < 2u ||
         config->audio_specific_config_bytes > 64u)) return -EINVAL;
    player_audio_decoder *d = calloc(1, sizeof(*d));
    if (!d) return -ENOMEM;
    d->codec = config->codec;
    if (d->codec == PLAYER_AUDIO_CODEC_MP3) {
        mp3dec_init(&d->backend.mp3);
    } else {
        d->backend.aac = NeAACDecOpen();
        if (!d->backend.aac) { free(d); return -ENOMEM; }
        NeAACDecConfigurationPtr cfg = NeAACDecGetCurrentConfiguration(d->backend.aac);
        cfg->defObjectType = LC;
        cfg->outputFormat = FAAD_FMT_16BIT;
        cfg->dontUpSampleImplicitSBR = 1;
        cfg->downMatrix = 0;
        if (!NeAACDecSetConfiguration(d->backend.aac, cfg)) goto unsupported;
        if (d->codec == PLAYER_AUDIO_CODEC_AAC_RAW) {
            mp4AudioSpecificConfig asc;
            memset(&asc, 0, sizeof(asc));
            if (NeAACDecAudioSpecificConfig((unsigned char *)config->audio_specific_config,
                    config->audio_specific_config_bytes, &asc) != 0 ||
                asc.objectTypeIndex != LC || asc.frameLengthFlag || asc.sbr_present_flag > 0 ||
                asc.forceUpSampling || !format_valid((uint32_t)asc.samplingFrequency,
                                                     asc.channelsConfiguration)) goto unsupported;
            unsigned long rate;
            unsigned char channels;
            if (NeAACDecInit2(d->backend.aac, (unsigned char *)config->audio_specific_config,
                    config->audio_specific_config_bytes, &rate, &channels) != 0 ||
                !format_valid((uint32_t)rate, channels)) goto unsupported;
            d->rate = (uint32_t)rate;
            d->channels = channels;
            d->initialized = 1;
        }
    }
    *output = d;
    return 0;
unsupported:
    player_audio_decoder_destroy(d);
    return -ENOTSUP;
}

int player_audio_decoder_decode(player_audio_decoder *d, const uint8_t *packet,
                                 uint32_t bytes, player_audio_decoded_frame *frame)
{
    if (frame) memset(frame, 0, sizeof(*frame));
    if (!d || !packet || !frame || !bytes || bytes > INT_MAX) return -EINVAL;
    if (d->codec == PLAYER_AUDIO_CODEC_MP3) {
        mp3dec_frame_info_t info = {0};
        int frames = mp3dec_decode_frame(&d->backend.mp3, packet, (int)bytes, d->mp3_pcm, &info);
        if (info.frame_bytes != (int)bytes || info.frame_offset || info.layer != 3 ||
            frames < 0 || !format_valid((uint32_t)info.hz, (uint32_t)info.channels)) return -EILSEQ;
        if (d->initialized && (d->rate != (uint32_t)info.hz || d->channels != (uint32_t)info.channels))
            return -ENOTSUP;
        d->initialized = 1;
        d->rate = (uint32_t)info.hz;
        d->channels = (uint32_t)info.channels;
        frame->pcm = d->mp3_pcm;
        frame->frames = (uint32_t)frames;
        frame->channels = d->channels;
        frame->sample_rate = d->rate;
        frame->consumed_bytes = bytes;
    } else {
        if (d->codec == PLAYER_AUDIO_CODEC_AAC_ADTS) {
            if (bytes < 7u || bytes < ((packet[1] & 1u) ? 7u : 9u) ||
                packet[0] != 0xffu || (packet[1] & 0xf6u) != 0xf0u ||
                (packet[2] >> 6) != 1u || (packet[6] & 3u) ||
                (((uint32_t)(packet[3] & 3u) << 11) | ((uint32_t)packet[4] << 3) |
                    (packet[5] >> 5)) != bytes) return -EILSEQ;
            uint32_t channels = ((packet[2] & 1u) << 2) | (packet[3] >> 6);
            if (channels != 1u && channels != 2u) return -ENOTSUP;
            if (!d->initialized) {
                unsigned long rate;
                unsigned char decoded_channels;
                if (NeAACDecInit(d->backend.aac, (unsigned char *)packet, bytes,
                        &rate, &decoded_channels) != 0 ||
                    !format_valid((uint32_t)rate, decoded_channels)) return -ENOTSUP;
                d->rate = (uint32_t)rate;
                d->channels = decoded_channels;
                d->initialized = 1;
            }
        }
        NeAACDecFrameInfo info;
        void *pcm = NeAACDecDecode(d->backend.aac, &info, (unsigned char *)packet, bytes);
        if (info.error || !pcm || info.bytesconsumed != bytes || info.samples > 2048u ||
            info.object_type != LC || info.sbr || info.ps || !format_valid((uint32_t)info.samplerate, info.channels) ||
            info.samplerate != d->rate || info.channels != d->channels || info.samples % info.channels)
            return -EILSEQ;
        frame->pcm = pcm;
        frame->frames = (uint32_t)info.samples / info.channels;
        frame->channels = info.channels;
        frame->sample_rate = (uint32_t)info.samplerate;
        frame->consumed_bytes = bytes;
        if (!d->calls && !info.samples) frame->decoder_skipped_frames = 1024;
    }
    ++d->calls;
    return 0;
}

void player_audio_decoder_destroy(player_audio_decoder *d)
{
    if (!d) return;
    if (d->codec != PLAYER_AUDIO_CODEC_MP3 && d->backend.aac) NeAACDecClose(d->backend.aac);
    free(d);
}

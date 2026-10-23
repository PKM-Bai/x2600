#include "ai_air1103.h"
#include "ai_ptt.h"
#include "ai_resampler.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(condition) do { if (!(condition)) { \
    fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition); \
    exit(1); } } while (0)

typedef struct {
    uint32_t frames;
    uint16_t command;
    size_t length;
    uint8_t payload[AI_AIR1103_MAX_PAYLOAD];
} received_frame;

static void receive(void *context, uint16_t command, const uint8_t *payload, size_t length)
{
    received_frame *r = context;
    ++r->frames;
    r->command = command;
    r->length = length;
    memcpy(r->payload, payload, length);
}

static void test_protocol(void)
{
    uint8_t pcm[512], frame[AI_AIR1103_MAX_FRAME], encoded[AI_AIR1103_MAX_FRAME];
    const uint8_t volume[] = {0x55, 0xaa, 0x00, 0x01, 0x02, 0x03, 0x1f, 0x24};
    const uint8_t stop[] = {0x55, 0xaa, 0x00, 0x00, 0x02, 0x01, 0x02};
    const uint8_t reset[] = {0x55, 0xaa, 0x00, 0x01, 0x02, 0x05, 0x00, 0x07};
    uint8_t value = 31;
    CHECK(ai_air1103_pack(AI_AIR1103_VOLUME, &value, 1, encoded, sizeof(encoded)) == sizeof(volume));
    CHECK(memcmp(encoded, volume, sizeof(volume)) == 0);
    CHECK(ai_air1103_pack(AI_AIR1103_STOP_MIC, NULL, 0, encoded, sizeof(encoded)) == sizeof(stop));
    CHECK(memcmp(encoded, stop, sizeof(stop)) == 0);
    value = 0;
    CHECK(ai_air1103_pack(AI_AIR1103_RESET, &value, 1, encoded, sizeof(encoded)) == sizeof(reset));
    CHECK(memcmp(encoded, reset, sizeof(reset)) == 0);
    value = 32;
    CHECK(ai_air1103_pack(AI_AIR1103_VOLUME, &value, 1, encoded, sizeof(encoded)) == 0);
    CHECK(ai_air1103_pack(AI_AIR1103_PLAY, pcm, 319, encoded, sizeof(encoded)) == 0);
    CHECK(ai_air1103_pack(AI_AIR1103_MIC, pcm, 511, encoded, sizeof(encoded)) == 0);
    CHECK(ai_air1103_pack(AI_AIR1103_MIC, NULL, 512, encoded, sizeof(encoded)) == 0);
    memset(pcm, 0, sizeof(pcm));
    CHECK(ai_air1103_pack(AI_AIR1103_PLAY, pcm, 320, frame, sizeof(frame)) == 327);
    CHECK(frame[2] == 1 && frame[3] == 0x40 && frame[326] == 0xe1);
    CHECK(ai_air1103_pack(AI_AIR1103_MIC, pcm, 512, frame, sizeof(frame)) == 519);
    CHECK(frame[2] == 2 && frame[3] == 0 && frame[518] == 0xa1);
    for (size_t i = 0; i < sizeof(pcm); ++i) pcm[i] = (uint8_t)i;
    memcpy(pcm + 30, volume, sizeof(volume)); /* A payload header is not a frame. */
    CHECK(ai_air1103_pack(AI_AIR1103_MIC, pcm, 512, frame, sizeof(frame)) == sizeof(frame));
    CHECK(ai_air1103_pack(AI_AIR1103_MIC, pcm, 512, encoded, sizeof(encoded) - 1) == 0);

    /* Every possible single split, including both empty halves. */
    for (size_t split = 0; split <= sizeof(frame); ++split) {
        ai_air1103_parser parser;
        received_frame r = {0};
        ai_air1103_init(&parser, receive, &r);
        CHECK(ai_air1103_feed(&parser, frame, split) == 0);
        CHECK(ai_air1103_feed(&parser, frame + split, sizeof(frame) - split) == 0);
        CHECK(r.frames == 1 && r.command == AI_AIR1103_MIC && r.length == 512);
        CHECK(memcmp(r.payload, pcm, sizeof(pcm)) == 0);
        CHECK(parser.used == 0 && parser.stats.checksum_errors == 0);
    }
    ai_air1103_parser parser;
    received_frame r = {0};
    ai_air1103_init(&parser, receive, &r);
    for (size_t i = 0; i < sizeof(frame); ++i) CHECK(ai_air1103_feed(&parser, frame + i, 1) == 0);
    CHECK(r.frames == 1);

    uint8_t combined[2 * AI_AIR1103_MAX_FRAME + 16];
    memcpy(combined, frame, sizeof(frame));
    memcpy(combined + sizeof(frame), frame, sizeof(frame));
    CHECK(ai_air1103_feed(&parser, combined, 2 * sizeof(frame)) == 0);
    CHECK(r.frames == 3);
    combined[0] = 0x01; combined[1] = 0x55;
    memcpy(combined + 2, frame, sizeof(frame));
    CHECK(ai_air1103_feed(&parser, combined, sizeof(frame) + 2) == 0);
    CHECK(r.frames == 4 && parser.stats.discarded_bytes == 2);

    /* Bad checksum followed by a valid frame in the same callback. */
    memset(pcm, 0, sizeof(pcm));
    CHECK(ai_air1103_pack(AI_AIR1103_MIC, pcm, 512, frame, sizeof(frame)) == sizeof(frame));
    memcpy(combined, frame, sizeof(frame));
    combined[sizeof(frame) - 1] ^= 1;
    memcpy(combined + sizeof(frame), frame, sizeof(frame));
    CHECK(ai_air1103_feed(&parser, combined, 2 * sizeof(frame)) == 0);
    CHECK(r.frames == 5 && parser.stats.checksum_errors == 1);
    const uint8_t oversized[] = {0x55, 0xaa, 0xff, 0xff};
    CHECK(ai_air1103_feed(&parser, oversized, sizeof(oversized)) == 0);
    CHECK(ai_air1103_feed(&parser, frame, sizeof(frame)) == 0);
    CHECK(r.frames == 6 && parser.stats.length_errors == 1);
    const uint8_t wrong_mic_length[] = {0x55, 0xaa, 0, 1, 0x20, 0x80};
    CHECK(ai_air1103_feed(&parser, wrong_mic_length, sizeof(wrong_mic_length)) == 0);
    CHECK(ai_air1103_feed(&parser, frame, sizeof(frame)) == 0);
    CHECK(r.frames == 7 && parser.stats.length_errors == 2);
    /* A truncated packet must not swallow the following complete packet. */
    memcpy(combined, frame, 10);
    memcpy(combined + 10, frame, sizeof(frame));
    CHECK(ai_air1103_feed(&parser, combined, sizeof(frame) + 10) == 0);
    CHECK(r.frames == 8 && parser.stats.checksum_errors == 2 && parser.used == 0);
    CHECK(ai_air1103_feed(&parser, NULL, 1) == -1);
    CHECK(ai_air1103_feed(&parser, NULL, 0) == 0);
    puts("PASS protocol: golden wire vectors, all splits, byte feed, concatenation, checksum/length resync");
}

static size_t convert(const int16_t *input, size_t count, int16_t *output,
                      size_t input_chunk, size_t output_chunk)
{
    ai_resampler r;
    ai_resampler_init(&r);
    size_t read = 0, written = 0;
    while (read < count) {
        size_t amount = count - read;
        if (amount > input_chunk) amount = input_chunk;
        size_t consumed, produced;
        CHECK(ai_resampler_process(&r, input + read, amount, &consumed,
                                   output + written, output_chunk, &produced) == 0);
        CHECK(consumed || produced);
        read += consumed;
        written += produced;
    }
    int finished;
    do {
        size_t produced;
        finished = ai_resampler_finish(&r, output + written, output_chunk, &produced);
        CHECK(finished >= 0);
        written += produced;
    } while (!finished);
    CHECK(written == ai_resampler_output_count(count));
    size_t produced;
    CHECK(ai_resampler_finish(&r, NULL, 0, &produced) == 1 && produced == 0);
    size_t consumed;
    CHECK(ai_resampler_process(&r, NULL, 0, &consumed, NULL, 0, &produced) == -1);
    return written;
}

static void test_resampler(void)
{
    int16_t input[997], whole[700], chunked[700];
    uint32_t random = 1234;
    for (size_t i = 0; i < 997; ++i) {
        random = random * 1664525u + 1013904223u;
        input[i] = (int16_t)(random >> 16);
    }
    for (size_t n = 0; n < 90; ++n) {
        size_t a = convert(input, n, whole, 997, 700);
        size_t b = convert(input, n, chunked, 1, 1);
        CHECK(a == n * 2 / 3 && a == b);
        CHECK(memcmp(whole, chunked, a * sizeof(*whole)) == 0);
    }
    size_t count = convert(input, 997, whole, 997, 700);
    const size_t chunks[] = {1, 2, 3, 7, 31, 48, 64, 161, 256};
    for (size_t i = 0; i < sizeof(chunks) / sizeof(chunks[0]); ++i) {
        CHECK(convert(input, 997, chunked, chunks[i], 1 + i * 3) == count);
        CHECK(memcmp(whole, chunked, count * sizeof(*whole)) == 0);
    }
    ai_resampler r;
    ai_resampler_init(&r);
    size_t consumed = 99, produced = 99;
    CHECK(ai_resampler_process(&r, input, 997, &consumed, NULL, 0, &produced) == 0);
    CHECK(consumed == 0 && produced == 0 && r.input_samples == 0);
    CHECK(ai_resampler_process(&r, NULL, 1, &consumed, whole, 1, &produced) == -1);

    const size_t in_count = 24000, out_count = 16000;
    int16_t *tone = malloc(in_count * sizeof(*tone));
    int16_t *converted = malloc(out_count * sizeof(*converted));
    CHECK(tone && converted);
    const double frequencies[] = {1000, 9000, 10000};
    double gain[3];
    for (unsigned f = 0; f < 3; ++f) {
        for (size_t i = 0; i < in_count; ++i)
            tone[i] = (int16_t)lround(12000.0 * sin(6.2831853071795864769 * frequencies[f] * (double)i / 24000.0));
        CHECK(convert(tone, in_count, converted, 113, 17) == out_count);
        double energy = 0;
        for (size_t i = 100; i < out_count - 100; ++i)
            energy += (double)converted[i] * converted[i];
        gain[f] = sqrt(energy / (double)(out_count - 200)) / (12000.0 / sqrt(2.0));
    }
    printf("FIR measured: 1kHz gain=%.6f, 9kHz alias=%.2fdB, 10kHz alias=%.2fdB\n",
           gain[0], 20 * log10(gain[1] + 1e-12), 20 * log10(gain[2] + 1e-12));
    CHECK(gain[0] > 0.98 && gain[0] < 1.02);
    CHECK(gain[1] < 0.005 && gain[2] < 0.005); /* >46dB alias rejection. */
    for (size_t i = 0; i < in_count; ++i) tone[i] = 32767;
    CHECK(convert(tone, in_count, converted, 97, 31) == out_count);
    for (size_t i = 100; i < out_count - 100; ++i) CHECK(converted[i] == 32767);
    free(tone); free(converted);
    puts("PASS resampler: short/long exact counts, chunk/capacity invariance, finish, passband/alias/DC");
}

static void start_response(ai_ptt *p, uint32_t now)
{
    CHECK(ai_ptt_press(p, now) == AI_PTT_START_CAPTURE);
    uint32_t accepted;
    CHECK(ai_ptt_capture(p, 256, &accepted) == 0 && accepted == 256);
    CHECK(ai_ptt_release(p, now + 16) == (AI_PTT_STOP_CAPTURE | AI_PTT_SUBMIT_CAPTURE));
    CHECK(p->state == AI_PTT_WAITING && ai_ptt_response_current(p, p->generation));
}

static void test_ptt(void)
{
    ai_ptt p;
    ai_ptt_init(&p);
    CHECK(!p.tail_delay_known && p.state == AI_PTT_IDLE);
    CHECK(ai_ptt_set_tail_delay(&p, 80) == 0);
    start_response(&p, 0);
    uint32_t old = p.generation;
    CHECK(ai_ptt_playback_begin(&p, old));
    uint32_t actions = ai_ptt_press(&p, 30);
    CHECK(actions == (AI_PTT_STOP_PLAYBACK | AI_PTT_CANCEL_RESPONSE));
    CHECK(!ai_ptt_response_current(&p, old) && !ai_ptt_playback_begin(&p, old));
    CHECK(p.state == AI_PTT_STOPPING);
    CHECK(ai_ptt_tick(&p, 1000) == 0); /* Driver has not acknowledged stop. */
    ai_ptt_playback_stopped(&p, old, 1000);
    CHECK(!p.stop_pending); /* Stale acknowledgement cannot open the mic. */
    ai_ptt_playback_stopped(&p, p.generation, 1000);
    CHECK(ai_ptt_tick(&p, 1079) == 0);
    CHECK(ai_ptt_tick(&p, 1080) == AI_PTT_START_CAPTURE);
    CHECK(p.capture_started_ms == 1080);
    uint32_t accepted;
    CHECK(ai_ptt_capture(&p, AI_PTT_MAX_SAMPLES + 100, &accepted) ==
          (AI_PTT_STOP_CAPTURE | AI_PTT_SUBMIT_CAPTURE));
    CHECK(accepted == AI_PTT_MAX_SAMPLES && p.state == AI_PTT_WAITING && p.held);
    CHECK(ai_ptt_tick(&p, 40000) == 0 && ai_ptt_press(&p, 40000) == 0);
    CHECK(ai_ptt_capture(&p, 256, &accepted) == 0 && accepted == 0);
    CHECK(ai_ptt_release(&p, 40001) == 0);
    CHECK(ai_ptt_press(&p, 40002) == (AI_PTT_CANCEL_RESPONSE | AI_PTT_START_CAPTURE));
    CHECK(ai_ptt_release(&p, 40003) == (AI_PTT_STOP_CAPTURE | AI_PTT_DISCARD_CAPTURE));
    CHECK(p.state == AI_PTT_IDLE);

    /* Releasing while waiting for the speaker must not submit an empty turn. */
    start_response(&p, 41000);
    CHECK(ai_ptt_playback_begin(&p, p.generation));
    CHECK(ai_ptt_press(&p, 42000) == (AI_PTT_CANCEL_RESPONSE | AI_PTT_STOP_PLAYBACK));
    ai_ptt_playback_stopped(&p, p.generation, 42010);
    CHECK(ai_ptt_release(&p, 42020) == 0 && p.state == AI_PTT_IDLE);
    CHECK(ai_ptt_press(&p, 42030) == 0 && p.state == AI_PTT_STOPPING);
    CHECK(ai_ptt_tick(&p, 42090) == AI_PTT_START_CAPTURE);
    CHECK(ai_ptt_cancel(&p) == (AI_PTT_STOP_CAPTURE | AI_PTT_DISCARD_CAPTURE));
    CHECK(ai_ptt_press(&p, 42091) == 0); /* Cancel also requires physical release. */
    CHECK(ai_ptt_release(&p, 42092) == 0);

    /* Wall-clock limit works even when frames are missing and across wrap. */
    CHECK(ai_ptt_press(&p, UINT32_MAX - 10000u) == AI_PTT_START_CAPTURE);
    CHECK(ai_ptt_capture(&p, 256, &accepted) == 0);
    CHECK(ai_ptt_tick(&p, 19998u) == 0);
    CHECK(ai_ptt_tick(&p, 19999u) == (AI_PTT_STOP_CAPTURE | AI_PTT_SUBMIT_CAPTURE));
    CHECK(ai_ptt_tick(&p, 20000u) == 0);
    CHECK(ai_ptt_release(&p, 20001u) == 0);
    ai_ptt_response_end(&p, p.generation);
    CHECK(p.state == AI_PTT_IDLE);

    /* Unknown hardware timing never silently turns into a verified delay. */
    ai_ptt_init(&p);
    start_response(&p, 0);
    CHECK(ai_ptt_playback_begin(&p, p.generation));
    CHECK(ai_ptt_press(&p, 100) == (AI_PTT_STOP_PLAYBACK | AI_PTT_CANCEL_RESPONSE | AI_PTT_NEED_STOP_TIMING));
    ai_ptt_playback_stopped(&p, p.generation, 101);
    CHECK(ai_ptt_tick(&p, 100000) == 0 && p.state == AI_PTT_STOPPING);
    CHECK(ai_ptt_release(&p, 100001) == 0 && p.state == AI_PTT_IDLE);
    CHECK(ai_ptt_set_tail_delay(&p, 120) == 0);
    CHECK(ai_ptt_tick(&p, 100002) == 0 && !p.may_be_audible);
    start_response(&p, 100003);
    CHECK(ai_ptt_playback_begin(&p, p.generation));
    ai_ptt_playback_stopped(&p, p.generation, 100020);
    CHECK(!p.stop_pending); /* Stop ACK cannot clear active playback. */
    ai_ptt_playback_end(&p, p.generation, 100030);
    CHECK(p.state == AI_PTT_IDLE && p.may_be_audible);
    CHECK(ai_ptt_press(&p, 100050) == 0 && p.state == AI_PTT_STOPPING);
    CHECK(ai_ptt_tick(&p, 100150) == AI_PTT_START_CAPTURE);
    p.generation = UINT32_MAX;
    CHECK(ai_ptt_cancel(&p) == (AI_PTT_STOP_CAPTURE | AI_PTT_DISCARD_CAPTURE));
    CHECK(p.generation == 1);
    puts("PASS PTT: tail ACK/timing, early release, auto-submit/release gate, stale generation, wrap, cancel");
}

int main(void)
{
    test_protocol();
    test_resampler();
    test_ptt();
    puts("PASS all audio core tests (no UART hardware initialized)");
    return 0;
}

#include "h2645_parse.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned allocations;

#ifdef _MSC_VER
/* Host equivalent of the GCC bit-reader intrinsic; firmware uses GCC's own. */
int __builtin_ctz(unsigned value)
{
    int count = 0;
    while (!(value & 1u)) { ++count; value >>= 1; }
    return count;
}
#endif

void *av_realloc_array(void *pointer, size_t count, size_t size)
{
    if (!pointer) ++allocations;
    return realloc(pointer, count * size);
}

void av_freep(void *address)
{
    void **pointer = address;
    if (*pointer) { --allocations; free(*pointer); *pointer = NULL; }
}

void av_log(void *context, int level, const char *format, ...)
{
    (void)context; (void)level; (void)format;
}

static int require(int condition, const char *message)
{
    if (!condition) { fprintf(stderr, "FAIL %s\n", message); return 0; }
    return 1;
}

int main(void)
{
    /* Both common SPS timing escape sequences and repeated slice zero runs. */
    static const uint8_t escaped[] = {
        0x67, 0x42, 0x00, 0x1f, 0x00, 0x00, 0x03, 0x01,
        0x00, 0x00, 0x03, 0x00, 0x00, 0x03, 0x00, 0x80,
        0x00, 0x00, 0x00, 0x01, 0x68, 0x80
    };
    static const uint8_t expected[] = {
        0x67, 0x42, 0x00, 0x1f, 0x00, 0x00, 0x01,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x80
    };
    static const uint8_t packet[] = {
        0,0,0,1,0x09,0xf0,
        0,0,1,0x67,0x42,0,0x1f,0,0,3,1,0x80,
        0,0,0,1,0x68,0x80,
        0,0,1,0x65,0x80,
        /* The first 03 escapes the second, literal RBSP 03. */
        0,0,1,0x06,0,0,3,3,0x80
    };
    H2645NAL nal = { 0 };
    H2645Packet split = { 0 };
    uint8_t padded[sizeof(packet) + 64] = { 0 };
    unsigned index;
    int consumed = ff_h2645_extract_rbsp(escaped, sizeof(escaped), &nal, 0);
    if (!require(consumed == 16 && nal.raw_size == 16, "consume full escaped NAL, stop at next prefix") ||
        !require(nal.size == (int)sizeof(expected) && nal.skipped_bytes == 3, "RBSP size and escapes") ||
        !require(!memcmp(nal.data, expected, sizeof(expected)), "complete unescaped bytes")) return 1;
    for (index = 0; index < 64; ++index)
        if (!require(nal.data[nal.size + index] == 0, "64 zero read-ahead padding")) return 1;
    av_freep(&nal.player_rbsp);
    memcpy(padded, packet, sizeof(packet));
    if (!require(!ff_h2645_packet_split(&split, padded, sizeof(packet), NULL, 0, 0, 0), "packet split") ||
        !require(split.nb_nals == 5, "AUD/SPS/PPS/IDR/SEI remain five NALs") ||
        !require(split.nals[0].type == 9 && split.nals[1].type == 7 &&
                 split.nals[2].type == 8 && split.nals[3].type == 5 && split.nals[4].type == 6, "NAL order") ||
        !require(split.nals[4].size == 5 && split.nals[4].data[3] == 3 &&
                 split.nals[4].data[4] == 0x80, "literal RBSP 00 00 03 survives exactly one unescape") ||
        !require(split.nals[1].raw_size == 9 && split.nals[1].size == 8, "SPS not truncated at escape")) return 1;
    if (!require(!ff_h2645_packet_split(&split, padded, sizeof(packet), NULL, 0, 0, 0), "buffer reuse")) return 1;
    ff_h2645_packet_uninit(&split);
    if (!require(!allocations, "all RBSP and NAL allocations released")) return 1;
    puts("PASS complete Annex B boundaries, RBSP escapes, 64 zero padding, reuse and teardown");
    return 0;
}

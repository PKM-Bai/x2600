#include <stdint.h>
#include <stdio.h>
#include <string.h>

#ifdef _MSC_VER
int __builtin_clz(unsigned value)
{
    int count = 0;
    if (!value) return 32;
    while (!(value & 0x80000000u)) { ++count; value <<= 1; }
    return count;
}
#endif

#include "h264_parse.h"
#include "mpegutils.h"
/* h264_parse.c needs no decoder context; excluding that header also avoids
 * its unused GNU empty SEI structure in this MSVC host translation unit. */
#define AVCODEC_H264DEC_H
#include "src/h264_parse.c"
#include "api/jzm_h264_dec.h"
#undef GEN_VDMA_ACFG
#define GEN_VDMA_ACFG(chn, reg, lk, val) do { \
    *chn++ = (val); \
    *chn++ = VDMA_ACFG_VLD | (lk) | VDMA_ACFG_IDX(reg); \
} while (0)
#include "api/jzm_h264_dec.c"

void av_log(void *context, int level, const char *format, ...)
{
    (void)context; (void)level; (void)format;
}

static uint32_t descriptors[8192];
static uint8_t bits[128];
static unsigned bit_count;

static void bit(unsigned value)
{
    bits[bit_count >> 3] |= (uint8_t)((value & 1u) << (7u - (bit_count & 7u)));
    ++bit_count;
}

static void ue(unsigned value)
{
    unsigned code = value + 1u, width = 0;
    for (unsigned copy = code; copy; copy >>= 1) ++width;
    for (unsigned index = 1; index < width; ++index) bit(0);
    for (unsigned index = width; index; --index) bit(code >> (index - 1u));
}

static void se(int value)
{
    ue(value <= 0 ? (unsigned)(-2 * value) : (unsigned)(2 * value - 1));
}

static int word(uint32_t reg, uint32_t expected)
{
    for (unsigned index = 0; index < sizeof(descriptors) / sizeof(*descriptors); index += 2) {
        uint32_t control = descriptors[index + 1];
        if ((control & 0xffffcu) == reg && descriptors[index] == expected) return 1;
        if (control & VDMA_ACFG_TERM) break;
    }
    fprintf(stderr, "FAIL prediction descriptor reg=%08x expected=%08x\n", reg, expected);
    return 0;
}

static int prediction(unsigned luma_denom)
{
    SPS sps = {0};
    H264PredWeightTable table = {0};
    GetBitContext reader;
    jzm_h264 hardware = {0};
    const int refs[2] = {3, 0};
    memset(bits, 0, sizeof(bits)); bit_count = 0;
    ue(luma_denom); ue(7);
    /* Real problematic P slice's reference 0: Y=55/+2 at denom 6,
     * U=112/+16 and V=111/+17 at denom 7. References 1/2 use defaults.
     * The second vector also exercises the luma identity 128 at denom 7. */
    bit(1); se(luma_denom == 6 ? 55 : 127); se(luma_denom == 6 ? 2 : -2);
    bit(1); se(112); se(16); se(111); se(17);
    bit(0); bit(0); bit(0); bit(0);
    sps.chroma_format_idc = 1;
    init_get_bits(&reader, bits, (int)bit_count);
    if (ff_h264_pred_weight_table(&reader, &sps, refs, AV_PICTURE_TYPE_P,
                                 &table, PICT_FRAME, NULL) < 0) return 0;
    for (unsigned ref = 1; ref < 3; ++ref) {
        if (table.luma_weight[ref][0][0] != (1 << luma_denom) ||
            table.luma_weight[ref][0][1] ||
            table.chroma_weight[ref][0][0][0] != 128 ||
            table.chroma_weight[ref][0][1][0] != 128 ||
            table.chroma_weight[ref][0][0][1] ||
            table.chroma_weight[ref][0][1][1]) {
            fprintf(stderr, "FAIL implicit prediction identity denom=%u ref=%u\n", luma_denom, ref);
            return 0;
        }
    }
    hardware.des_va = (int *)descriptors;
    hardware.slice_type = JZM_H264_P_TYPE;
    hardware.mb_width = 80; hardware.mb_height = 45;
    hardware.use_weight = table.use_weight;
    hardware.use_weight_chroma = table.use_weight_chroma;
    hardware.luma_log2_weight_denom = table.luma_log2_weight_denom;
    hardware.chroma_log2_weight_denom = table.chroma_log2_weight_denom;
    for (unsigned ref = 0; ref < 3; ++ref) {
        hardware.luma_weight[0][ref] = table.luma_weight[ref][0][0];
        hardware.luma_offset[0][ref] = table.luma_weight[ref][0][1];
        for (unsigned plane = 0; plane < 2; ++plane) {
            hardware.chroma_weight[0][ref][plane] = table.chroma_weight[ref][0][plane][0];
            hardware.chroma_offset[0][ref][plane] = table.chroma_weight[ref][0][plane][1];
        }
    }
    memset(descriptors, 0, sizeof(descriptors));
    jzm_h264_slice_init_vdma(&hardware);
    if (!word(REG_MCE_CH2_RLUT, 0x6f117010) ||
        !word(REG_MCE_CH1_RLUT, luma_denom == 6 ? 0x3702 : 0x7ffe)) return 0;
    for (unsigned ref = 1; ref < 3; ++ref)
        if (!word(REG_MCE_CH2_RLUT + ref * 8u, 0x80008000) ||
            !word(REG_MCE_CH1_RLUT + ref * 8u, luma_denom == 6 ? 0x4000 : 0x8000)) return 0;
    return 1;
}

int main(void)
{
    if (!prediction(6) || !prediction(7)) return 1;
    puts("PASS actual prediction-weight parser and VDMA: denom 7 identity 128, U/V offsets, three references");
    return 0;
}

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "api/jzm_h264_dec.h"

/* The vendor's statement expression is a statement at every call site.
 * Keep the descriptor writes identical while allowing MSVC's C compiler. */
#undef GEN_VDMA_ACFG
#define GEN_VDMA_ACFG(chn, reg, lk, val) do { \
    *chn++ = (val); \
    *chn++ = VDMA_ACFG_VLD | (lk) | VDMA_ACFG_IDX(reg); \
} while (0)
#include "api/jzm_h264_dec.c"

static uint32_t descriptors[8192];

static int require(int condition, const char *message)
{
    if (!condition) fprintf(stderr, "FAIL %s\n", message);
    return condition;
}

static int register_word(uint32_t reg, uint32_t *result)
{
    int found = 0;
    for (unsigned index = 0; index < sizeof(descriptors) / sizeof(*descriptors); index += 2) {
        uint32_t control = descriptors[index + 1];
        if ((control & 0xffffcu) == reg) {
            *result = descriptors[index];
            found = 1;
        }
        if (control & VDMA_ACFG_TERM) return found;
    }
    return 0;
}

int main(void)
{
    /* Fixed H.264 CABAC m,n vectors and SDE words at QP 0/26/51.
     * These fixed vectors include negative m, negative n, low/high clipping,
     * and all three PB initialization models. Context 276 is the special
     * termination context and is deliberately overwritten by the hardware API. */
    static const struct {
        unsigned context;
        uint32_t words[3];
    } intra[] = {
        {0, {0x0807060a, 0x15120f18, 0x6d5e4f82}}, /* m=20,n=-15 */
        {6, {0x0807060b, 0x62554775, 0x3d352c48}}, /* m=-28,n=127 */
        {70,{0x0f0d0b12, 0x0f0d0b12, 0x0f0d0b12}}, /* m=0,n=11 */
    };
    static const uint32_t pb[3][2][3] = {
        {{0x312a243a, 0xae977fd1, 0x1a16131f}, /* ctx11 m=23,n=33 */
         {0x4f443a5f, 0x62554775, 0x73635489}}, /* ctx105 m=-2,n=85 */
        {{0x201c1726, 0xccb195f4, 0x2f282239}, /* ctx11 m=22,n=25 */
         {0x1e1a1625, 0x62554775, 0xd7ba9dfe}}, /* ctx105 m=-13,n=103 */
        {{0x14110e16, 0xefcfaffe, 0x1714111b}, /* ctx11 m=29,n=16 */
         {0x4b41375b, 0x6d5e4f83, 0x95816db3}}, /* ctx105 m=-4,n=86 */
    };
    static const unsigned quantizers[] = {0, 26, 51};
    static const unsigned contexts[] = {11, 105};
    if (!require((char)-1 > 0, "test must reproduce MIPS unsigned plain-char default") ||
        !require(cabac_context_init_I[0][1] == -15 && cabac_context_init_I[6][0] == -28,
                 "CABAC I coefficients retain their negative signs") ||
        !require(cabac_context_init_PB[1][105][0] == -13,
                 "CABAC PB coefficients retain their negative signs")) return 1;
    unsigned checked = 0;
    for (unsigned type = 0; type < 3; ++type)
        for (unsigned model = 0; model < (type ? 3u : 1u); ++model)
            for (unsigned qp = 0; qp < 3; ++qp) {
                jzm_h264 hardware = {0};
                uint32_t value = 0;
                memset(descriptors, 0, sizeof(descriptors));
                hardware.des_va = (int *)descriptors;
                hardware.cabac = 1;
                hardware.slice_type = type == 0 ? JZM_H264_I_TYPE :
                                      type == 1 ? JZM_H264_P_TYPE : JZM_H264_B_TYPE;
                hardware.qscale = (unsigned char)quantizers[qp];
                hardware.cabac_init_idc = (int)model;
                hardware.mb_width = 80; hardware.mb_height = 45;
                jzm_h264_slice_init_vdma(&hardware);
                unsigned count = type ? 2u : 3u;
                for (unsigned index = 0; index < count; ++index) {
                    unsigned context = type ? contexts[index] : intra[index].context;
                    uint32_t expected = type ? pb[model][index][qp] : intra[index].words[qp];
                    if (!register_word(REG_SDE_CTX_TBL + context * 4u, &value) || value != expected) {
                        fprintf(stderr, "FAIL type=%u model=%u qp=%u ctx=%u actual=%08x expected=%08x\n",
                                type, model, quantizers[qp], context, value, expected);
                        return 1;
                    }
                    ++checked;
                }
                if (!require(register_word(REG_SDE_CTX_TBL + 276u * 4u, &value) && value == 0x01010102,
                             "CABAC termination context is forced to state 126")) return 1;
            }
    printf("PASS unsigned-char CABAC I/P/B descriptor contexts: %u golden words, QP 0/26/51\n", checked);
    return 0;
}

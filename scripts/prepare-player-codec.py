#!/usr/bin/env python3
"""Derive the player's FELIX/libh264 sources without editing the vendor SDK."""
from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import re

ROOT = Path(__file__).resolve().parents[1]
SDK = ROOT / "libbare-cpu/drivers/drivers-x2000/src/felix/libh264"


def replace_once(text: str, old: str, new: str, path: str) -> str:
    if text.count(old) != 1:
        raise ValueError(f"{path}: vendor patch anchor changed: {old[:70]!r}")
    return text.replace(old, new, 1)


def prepare(destination: Path) -> None:
    changes = {}
    for source in sorted(SDK.rglob("*")):
        if not source.is_file() or source.suffix not in (".c", ".h"):
            continue
        relative = source.relative_to(SDK).as_posix()
        original = source.read_bytes()
        text = original.decode("utf-8-sig").replace("\r\n", "\n")
        if relative == "include/frame.h":
            text = replace_once(text, "typedef unsigned int size_t;", "#include <stddef.h>", relative)
        elif relative == "include/h264dec.h":
            text = replace_once(text, "    AVCodecContext *avctx;", "    AVCodecContext *avctx;\n    void *player_owner;", relative)
        elif relative == "include/h264_ps.h":
            text = replace_once(text, "    int num_reorder_frames;", "    int num_reorder_frames;\n    int max_dec_frame_buffering;", relative)
            text = replace_once(text, """#if 0
    enum AVColorPrimaries color_primaries;
    enum AVColorTransferCharacteristic color_trc;
    enum AVColorSpace colorspace;
#endif""", """    /* Original ISO/H264 VUI numbers; display policy lives outside libh264. */
    int colour_primaries;
    int transfer_characteristics;
    int matrix_coefficients;""", relative)
        elif relative == "include/h264_common.h":
            text = replace_once(text, "return av_popcount((uint32_t)x) + av_popcount((uint32_t)(x >> 32));",
                                "return av_popcount_c((uint32_t)x) + av_popcount_c((uint32_t)(x >> 32));", relative)
        elif relative == "include/h2645_parse.h":
            text = replace_once(text, "    int size;", "    int size;\n    uint8_t *player_rbsp;\n    unsigned int player_rbsp_capacity;", relative)
        elif relative == "src/h2645_parse.c":
            begin = text.index("int ff_h2645_extract_rbsp(")
            end = text.index("\nstatic const char *hevc_nal_type_name", begin)
            text = text[:begin] + '''int ff_h2645_extract_rbsp(const uint8_t *src, int length,
                          H2645NAL *nal, int small_padding)
{
    int consumed = 0, output = 0, zeros = 0, i;
    uint8_t *buffer;
    (void)small_padding;
    /* 00 00 03 is emulation prevention, not the end of a NAL. */
    while (consumed < length) {
        if (consumed + 2 < length && src[consumed] == 0 && src[consumed + 1] == 0 &&
            (src[consumed + 2] == 1 || (consumed + 3 < length &&
             src[consumed + 2] == 0 && src[consumed + 3] == 1))) break;
        ++consumed;
    }
    if (!consumed) return AVERROR_INVALIDDATA;
    if ((unsigned int)consumed + AV_INPUT_BUFFER_PADDING_SIZE > nal->player_rbsp_capacity) {
        buffer = av_realloc_array(nal->player_rbsp, 1, (unsigned int)consumed + AV_INPUT_BUFFER_PADDING_SIZE);
        if (!buffer) return AVERROR(ENOMEM);
        nal->player_rbsp = buffer;
        nal->player_rbsp_capacity = (unsigned int)consumed + AV_INPUT_BUFFER_PADDING_SIZE;
    }
    nal->skipped_bytes = 0;
    for (i = 0; i < consumed; ++i) {
        if (zeros >= 2 && src[i] == 3 && i + 1 < consumed && src[i + 1] <= 3) {
            ++nal->skipped_bytes;
            zeros = 0;
            continue;
        }
        nal->player_rbsp[output++] = src[i];
        zeros = src[i] == 0 ? zeros + 1 : 0;
    }
    memset(nal->player_rbsp + output, 0, AV_INPUT_BUFFER_PADDING_SIZE);
    nal->data = nal->player_rbsp;
    nal->size = output;
    nal->raw_data = src;
    nal->raw_size = consumed;
    return consumed;
}
''' + text[end:]
            begin = text.index("static int h264_parse_nal_header(")
            end = text.index("\nstatic int find_next_start_code", begin)
            text = text[:begin] + '''static int h264_parse_nal_header(H2645NAL *nal, void *logctx)
{
    GetBitContext *gb = &nal->gb;
    (void)logctx;
    if (get_bits1(gb) != 0)
        return AVERROR_INVALIDDATA;
    nal->ref_idc = get_bits(gb, 2);
    nal->type = get_bits(gb, 5);
    /* Extraction already produced RBSP for every NAL, including SPS/PPS/SEI. */
    return 1;
}
''' + text[end:]
            text = replace_once(text, "    av_freep(&pkt->nals);", "    for (i = 0; i < pkt->nals_allocated; ++i)\n        av_freep(&pkt->nals[i].player_rbsp);\n    av_freep(&pkt->nals);", relative)
        elif relative == "src/h264_slice.c":
            text = '#include "player_codec_hooks.h"\n' + text
            reference_helper = '''static int player_set_reference_buffers(H264SliceContext *sl, jzm_h264 *hardware)
{
    unsigned int list, index;
    if (sl->list_count < 0 || sl->list_count > 2) return AVERROR_INVALIDDATA;
    hardware->ref_frm_ctrl = hardware->ref_frm_mv = 0;
    memset(hardware->mc_ref_y, 0, sizeof(hardware->mc_ref_y));
    memset(hardware->mc_ref_c, 0, sizeof(hardware->mc_ref_c));
    /* Unused entries can still point to an already released DPB picture.
     * Only active lists/references are legal hardware inputs. */
    for (list = 0; list < (unsigned int)sl->list_count; ++list) {
        if (sl->ref_count[list] > 16u) return AVERROR_INVALIDDATA;
        for (index = 0; index < sl->ref_count[list]; ++index) {
            H264Picture *parent = sl->ref_list[list][index].parent;
            if (!parent || !parent->f || !parent->dec_result_y || !parent->dec_result_uv ||
                !parent->frm_info_ctrl || !parent->frm_info_mv) return AVERROR_INVALIDDATA;
            hardware->mc_ref_y[list][index] = parent->dec_result_y->buffer_pa;
            hardware->mc_ref_c[list][index] = parent->dec_result_uv->buffer_pa;
        }
    }
    if (sl->list_count > 1 && sl->ref_count[1]) {
        H264Picture *colocated = sl->ref_list[1][0].parent;
        hardware->ref_frm_ctrl = colocated->frm_info_ctrl->buffer_pa;
        hardware->ref_frm_mv = colocated->frm_info_mv->buffer_pa;
    }
    return 0;
}

'''
            text = replace_once(text, "static int decode_slice(struct AVCodecContext *avctx, void *arg)",
                                reference_helper + "static int decode_slice(struct AVCodecContext *avctx, void *arg)", relative)
            text = replace_once(text, '''    if (sl->ref_list[1][0].parent) {
        st_h264->ref_frm_ctrl                = sl->ref_list[1][0].parent->frm_info_ctrl->buffer_pa;
    }
    if (sl->ref_list[1][0].parent) {
        st_h264->ref_frm_mv                  = sl->ref_list[1][0].parent->frm_info_mv->buffer_pa;
    }''', '''    ret = player_set_reference_buffers(sl, st_h264);
    if (ret < 0) return ret;''', relative)
            text = replace_once(text, '''        H264Picture *parent;
        parent = sl->ref_list[0][i].parent;

        st_h264->mc_ref_y[0][i] = parent ? parent->dec_result_y->buffer_pa : 0;
        st_h264->mc_ref_c[0][i] = parent ? parent->dec_result_uv->buffer_pa : 0;

        parent = sl->ref_list[1][i].parent;
        st_h264->mc_ref_y[1][i] = parent ? parent->dec_result_y->buffer_pa : 0;
        st_h264->mc_ref_c[1][i] = parent ? parent->dec_result_uv->buffer_pa : 0;

''', "", relative)
            text = replace_once(text, "    if (h->width > h->coded_width || h->height > h->coded_height) {",
                                "    if (sl->ref_count[0] > 16u || sl->ref_count[1] > 16u)\n        return AVERROR(EINVAL);\n\n    if (h->width > h->coded_width || h->height > h->coded_height) {", relative)
            text = replace_once(text, "if (!out_of_order /* && pics > h->avctx->has_b_frames*/)",
                                "if (!out_of_order && pics > h->avctx->has_b_frames)", relative)
            begin = text.index("static void release_picture(")
            end = text.index("\nvoid ff_h264_release_picture(", begin)
            text = text[:begin] + '''static void release_picture(H264Context *h, H264Picture *pic)
{
    if (!pic->f) return;
    /* The raw DPB workspace belongs to this codec for the whole session.
     * DPB and DPU hold the external linear frame independently. */
    player_codec_picture_release(h, pic);
    player_codec_frame_release(h, pic->f);
    pic->f = NULL;
}
''' + text[end:]
            begin = text.index("static int alloc_picture(")
            end = text.index("\nstatic int find_unused_picture(", begin)
            text = text[:begin] + '''static int alloc_picture(H264Context *h, H264Picture *pic)
{
    AVFrame *frame = player_codec_frame_acquire(h);
    int result;
    if (!frame) return AVERROR(ENOMEM);
    /* The hook supplies byte strides and persistent macroblock/tiled data;
     * no per-picture allocation can fragment the constrained DDR heap. */
    result = player_codec_picture_acquire(h, pic, frame);
    if (result < 0) {
        player_codec_frame_release(h, frame);
        return result;
    }
    pic->f = frame;
    return 0;
}
''' + text[end:]
            begin = text.index("    unsigned char slice_header[MAX_SLICE_HEADER_SIZE];")
            end = text.index("    //dump_slice_header(sl);", begin)
            text = text[:begin] + '''    /* The NAL parser owns a complete de-escaped RBSP. Parse its bounded
     * bitstream directly: re-copying 32 bytes truncates long reference-list
     * headers and a second unescape pass can remove real RBSP payload. */
    sl->gb = nal->gb;
    ret = h264_slice_header_parse(h, sl, nal);
    if (ret < 0)
        return ret;

''' + text[end:]
            text = replace_once(text, "st_h264->frm_y_stride = ROUND_UP(h->width, 128);",
                                "st_h264->frm_y_stride = cur_pic->f->linesize[0];", relative)
            text = replace_once(text, "        //return AVERROR(EINVAL);", "        return ret;", relative)
            text = replace_once(text, "    jzm_h264_slice_init_vdma(st_h264);",
                                "    player_codec_prepare_weights(h, sl);\n    jzm_h264_slice_init_vdma(st_h264);", relative)
            text = replace_once(text, "    vpu_hw_wait(&h->vpu_ctx);", "    ret = vpu_hw_wait(&h->vpu_ctx);\n    if (ret < 0) return ret;", relative)
            begin = text.index("#if 1   /* 在M200上需要处理以下bs_addr")
            end = text.index("\n#endif", begin) + len("\n#endif")
            text = text[:begin] + '''    /* ff_h2645_extract_rbsp already removed emulation prevention bytes.
     * CABAC starts after cabac_alignment_one_bit up to the next byte; CAVLC
     * starts at the exact slice-header bit. Both use the same owned RBSP and
     * must leave the VPU's second emulation-prevention pass disabled. */
    if (st_h264->cabac)
        st_h264->bs_index = (st_h264->bs_index + 7u) & ~7u;
    unsigned int bs_addr = (st_h264->bs_buffer + (st_h264->bs_index >> 3)) & ~3u;
    unsigned int bs_ofst = (((st_h264->bs_buffer & 3u) << 3) + st_h264->bs_index) & 31u;
    st_h264->bs_size_in_bits = st_h264->bs_size_in_bits - st_h264->bs_index + bs_ofst;
    st_h264->bs_buffer = bs_addr;
    st_h264->bs_index = bs_ofst;
    st_h264->bs_rbsp_en = 0;
''' + text[end:]
        elif relative == "src/h264_parse.c":
            # The implicit prediction weight is exactly 1 << denom.  At
            # denom 7 it is 128, although explicitly coded weights are int8.
            # Clamping this default to 127 darkens weighted reference planes.
            text = replace_once(text, "                if (luma_def > 127) {\n                    luma_def = 127;\n                }\n", "", relative)
            text = replace_once(text, "                        if (chroma_def > 127) {\n                            chroma_def = 127;\n                        }\n", "", relative)
        elif relative == "src/h264_ps.c":
            text = replace_once(text, "    sps->full_range           = -1;",
                                "    sps->full_range           = -1;\n    sps->bitstream_restriction_flag = 0;\n    sps->num_reorder_frames = 0;\n    sps->max_dec_frame_buffering = 0;", relative)
            text = replace_once(text, "        get_ue_golomb(gb); /* max_dec_frame_buffering */",
                                "        sps->max_dec_frame_buffering = get_ue_golomb(gb);", relative)
            text = replace_once(text, """            get_bits(gb, 8); /* colour_primaries */
            get_bits(gb, 8); /* transfer_characteristics */
            get_bits(gb, 8); /* matrix_coefficients */""", """            sps->colour_primaries = get_bits(gb, 8);
            sps->transfer_characteristics = get_bits(gb, 8);
            sps->matrix_coefficients = get_bits(gb, 8);""", relative)
            text = replace_once(text, "    sps->full_range           = -1;", """    sps->full_range           = -1;
    sps->video_signal_type_present_flag = 0;
    sps->colour_description_present_flag = 0;
    sps->colour_primaries = 2;
    sps->transfer_characteristics = 2;
    sps->matrix_coefficients = 2;
    sps->sar.num = sps->sar.den = 0;""", relative)
        elif relative == "src/h264dec.c":
            text = '#include "player_codec_hooks.h"\n' + text
            text = replace_once(text, "    if (buf_index < 0) {\n        return AVERROR_INVALIDDATA;\n    }",
                                "    if (buf_index < 0) {\n        return buf_index;\n    }", relative)
            text = replace_once(text, '            av_log(h->avctx, AV_LOG_ERROR, "decode_slice_header error\\n");\n            ret = AVERROR_INVALIDDATA;',
                                '            av_log(h->avctx, AV_LOG_ERROR, "decode_slice_header error\\n");\n            ret = err;', relative)
            text = replace_once(text, "    f->queued = 0;\n    INIT_LIST_HEAD(&f->done_entry);", """    if (player_codec_frame_publish(h, f) < 0)
        return AVERROR(EINVAL);
    f->queued = 0;
    INIT_LIST_HEAD(&f->done_entry);""", relative)
        elif relative == "api/jzm_h264_dec.c":
            # The MIPS compiler defaults plain char to unsigned. CABAC m/n
            # coefficients participate in signed arithmetic before clipping.
            text = replace_once(text, "__place_k0_data__ char cabac_context_init_I[460][2]",
                                "__place_k0_data__ signed char cabac_context_init_I[460][2]", relative)
            text = replace_once(text, "__place_k0_data__ char cabac_context_init_PB[3][460][2]",
                                "__place_k0_data__ signed char cabac_context_init_PB[3][460][2]", relative)
        elif relative == "src/mem.c":
            expression = r"void \*av_malloc_aligned\(size_t alignment, size_t size\)\n\{.*?\n\}\n\nvoid av_free_aligned\(void \*ptr\)\n\{.*?\n\}"
            text, count = re.subn(expression, """void *av_malloc_aligned(size_t alignment, size_t size)
{
    /* newlib memalign allocations support ordinary realloc/free. */
    return memalign(alignment, size ? size : 1);
}

void av_free_aligned(void *ptr)
{
    free(ptr);
}""", text, flags=re.S)
            if count != 1:
                raise ValueError("mem.c allocator anchor changed")
        elif relative == "src/buffer.c":
            text = replace_once(text, "    av_freep(buf);", "    av_free(buf);", relative)
        elif relative == "src/log.c":
            text = replace_once(text, "n = vsprintf(print_buffer, fmt, vl);",
                                "n = vsnprintf((char *)print_buffer, sizeof(print_buffer), fmt, vl);", relative)
            text = replace_once(text, "if (n > MAX_LOG_BUFFER_SIZE)", "if (n < 0 || n >= MAX_LOG_BUFFER_SIZE)", relative)
            text = replace_once(text, "printf(print_buffer);", 'printf("%s", (char *)print_buffer);', relative)
        target = destination / relative
        target.parent.mkdir(parents=True, exist_ok=True)
        if not target.exists() or target.read_text(encoding="utf-8") != text:
            target.write_text(text, encoding="utf-8", newline="\n")
        changes[relative] = dict(source_sha256=hashlib.sha256(original).hexdigest(),
                                 derived_sha256=hashlib.sha256(text.encode()).hexdigest())
    (destination / "manifest.json").write_text(json.dumps(changes, indent=2) + "\n", encoding="utf-8")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    prepare(args.output.resolve())

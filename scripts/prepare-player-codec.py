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
            text = replace_once(text, "    av_freep(&pkt->nals);", "    for (i = 0; i < pkt->nals_allocated; ++i)\n        av_freep(&pkt->nals[i].player_rbsp);\n    av_freep(&pkt->nals);", relative)
        elif relative == "src/h264_slice.c":
            text = '#include "player_codec_hooks.h"\n' + text
            old = """    pic->f->data[0] = NULL;
    pic->f->data[1] = NULL;
    pic->f->linesize[0] = 0;
    pic->f->linesize[1] = 0;
    pic->f = NULL;"""
            text = replace_once(text, old, """    /* DPB and DPU own independent holds on this external frame. */
    player_codec_frame_release(h, pic->f);
    pic->f = NULL;""", relative)
            text = replace_once(text, "f = list_first_entry_or_null(&h->queued_list, AVFrame, queued_entry);",
                                "f = player_codec_frame_acquire(h);", relative)
            text = replace_once(text, "f->linesize[0] = f->buf[0]->size;\n    f->linesize[1] = f->buf[1]->size;",
                                "f->linesize[0] = 768;\n    f->linesize[1] = 768;", relative)
            text = replace_once(text, "    list_del(&f->queued_entry);", "    /* Pool acquisition replaces the vendor's unowned queue. */", relative)
            text = replace_once(text, "fail:\n    ff_h264_unref_picture(h, pic);", "fail:\n    player_codec_frame_release(h, f);\n    ff_h264_unref_picture(h, pic);", relative)
            text = replace_once(text, "st_h264->frm_y_stride = ROUND_UP(h->width, 128);",
                                "st_h264->frm_y_stride = cur_pic->f->linesize[0];", relative)
            text = replace_once(text, "        //return AVERROR(EINVAL);", "        return ret;", relative)
            text = replace_once(text, "    vpu_hw_wait(&h->vpu_ctx);", "    ret = vpu_hw_wait(&h->vpu_ctx);\n    if (ret < 0) return ret;", relative)
            begin = text.index("#if 1   /* 在M200上需要处理以下bs_addr")
            end = text.index("\n#endif", begin) + len("\n#endif")
            text = text[:begin] + '''    /* The packet parser already removed emulation prevention bytes.
     * The fixed Baseline profile uses CAVLC, so VPU RBSP removal stays off. */
    if (st_h264->cabac) return AVERROR(EINVAL);
    unsigned int bs_addr = (st_h264->bs_buffer + (st_h264->bs_index >> 3)) & ~3u;
    unsigned int bs_ofst = (((st_h264->bs_buffer & 3u) << 3) + st_h264->bs_index) & 31u;
    st_h264->bs_size_in_bits = st_h264->bs_size_in_bits - st_h264->bs_index + bs_ofst;
    st_h264->bs_buffer = bs_addr;
    st_h264->bs_index = bs_ofst;
    st_h264->bs_rbsp_en = 0;
''' + text[end:]
        elif relative == "src/h264dec.c":
            text = '#include "player_codec_hooks.h"\n' + text
            text = replace_once(text, "    f->queued = 0;\n    INIT_LIST_HEAD(&f->done_entry);", """    if (player_codec_frame_publish(h, f) < 0)
        return AVERROR(EINVAL);
    f->queued = 0;
    INIT_LIST_HEAD(&f->done_entry);""", relative)
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

#ifndef PLAYER_CODEC_FORMAT_H
#define PLAYER_CODEC_FORMAT_H
#include "player_video.h"
#include "h264_ps.h"
#include <errno.h>
#include <string.h>

/* Private pure helper. The SPS values are parsed metadata, not DPU modes.
 * Only ordinary SDR transfer curves are accepted; no HDR tone mapping exists. */
static inline int player_codec_format_from_sps(const SPS *sps, player_video_format_t *out)
{
    player_video_format_t f;
    uint32_t width,height;
    if(!sps || !out) return -EINVAL;
    if(sps->mb_width<=0 || sps->mb_height<=0 || sps->mb_width>127 || sps->mb_height>127) return -ENOTSUP;
    width=(uint32_t)sps->mb_width*16u; height=(uint32_t)sps->mb_height*16u;
    if((uint64_t)width*height>1280u*720u || sps->crop_left>=width ||
       sps->crop_right>=width-sps->crop_left || sps->crop_top>=height ||
       sps->crop_bottom>=height-sps->crop_top) return -ENOTSUP;
    memset(&f,0,sizeof(f));
    f.coded_width=width; f.coded_height=height;
    f.visible_width=width-sps->crop_left-sps->crop_right;
    f.visible_height=height-sps->crop_top-sps->crop_bottom;
    f.crop_left=sps->crop_left; f.crop_top=sps->crop_top;
    if((f.visible_width&1u) || (f.visible_height&1u)) return -ENOTSUP;
    if(sps->sar.num<0 || sps->sar.den<0 || (sps->sar.num>0 && !sps->sar.den)) return -ENOTSUP;
    /* The SDK normalizes an unspecified SAR to0/1 after parsing. Public
     * metadata uses0/0 consistently so display can infer square pixels. */
    if(sps->sar.num>0) { f.sar_num=(uint32_t)sps->sar.num; f.sar_den=(uint32_t)sps->sar.den; }
    f.matrix_coefficients=2u; f.full_range=-1;
    f.colour_primaries=2u; f.transfer_characteristics=2u;
    if(sps->vui_parameters_present_flag && sps->video_signal_type_present_flag) {
        if(sps->full_range!=0 && sps->full_range!=1) return -ENOTSUP;
        f.full_range=sps->full_range;
        if(sps->colour_description_present_flag) {
            f.matrix_coefficients=(uint32_t)sps->matrix_coefficients;
            f.colour_primaries=(uint32_t)sps->colour_primaries;
            f.transfer_characteristics=(uint32_t)sps->transfer_characteristics;
        }
    }
    return player_video_format_resolve(&f,NULL,out);
}
#endif

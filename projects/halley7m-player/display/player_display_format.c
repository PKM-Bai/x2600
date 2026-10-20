#include "player_display_format.h"
#include <errno.h>
#include <string.h>

bool player_display_same_format(const player_video_format_t *a, const player_video_format_t *b)
{
    return a->coded_width==b->coded_width && a->coded_height==b->coded_height &&
           a->visible_width==b->visible_width && a->visible_height==b->visible_height &&
           a->crop_left==b->crop_left && a->crop_top==b->crop_top &&
           a->sar_num==b->sar_num && a->sar_den==b->sar_den &&
           a->matrix_coefficients==b->matrix_coefficients && a->full_range==b->full_range &&
           a->colour_primaries==b->colour_primaries && a->transfer_characteristics==b->transfer_characteristics;
}

int player_display_plan_format(const player_video_format_t *f, player_display_format_plan_t *p)
{
    uint32_t sn,sd,y,rv,gu,gv,bu;
    uint64_t dar_n,dar_d,scaled;
    int result;
    if(!f || !p) return -EINVAL;
    memset(p,0,sizeof(*p));
    player_video_format_t checked;
    result=player_video_format_resolve(f,NULL,&checked);
    if(result) return result;
    /* Stage4 remains the <=720p-pixel, progressive 8bit420 memory envelope.
     * The hardware max source/scaler dimension is 2047 (PM 11.1). */
    if(!f->coded_width || !f->coded_height || f->coded_width>2047 || f->coded_height>2047 ||
       (f->coded_width&15u) || (f->coded_height&15u) ||
       (uint64_t)f->coded_width*f->coded_height>1280u*720u ||
       !f->visible_width || !f->visible_height || (f->visible_width&1u) || (f->visible_height&1u) ||
       f->crop_left>f->coded_width || f->visible_width>f->coded_width-f->crop_left ||
       f->crop_top>f->coded_height || f->visible_height>f->coded_height-f->crop_top ||
       (f->crop_left&7u) || (f->crop_top&1u)) return -ENOTSUP;
    if((!f->sar_num)!=(!f->sar_den) || f->full_range < -1 || f->full_range > 1) return -EINVAL;
    sn=f->sar_num?f->sar_num:1u; sd=f->sar_den?f->sar_den:1u;
    dar_n=(uint64_t)f->visible_width*sn; dar_d=(uint64_t)f->visible_height*sd;
    p->format=*f;
    result=player_video_layout_nv12(f->coded_width,f->coded_height,&p->memory);
    if(result) return result;
    p->canvas_width=dar_n>=dar_d?1280u:720u;
    p->canvas_height=dar_n>=dar_d?720u:1280u;
    /* Cross products fit uint64: visible <=2047, SAR <=UINT32_MAX, canvas<=1280. */
    if(dar_n*p->canvas_height>=dar_d*p->canvas_width) {
        p->width=p->canvas_width;
        scaled=(uint64_t)p->width*dar_d/dar_n;
        p->height=(uint32_t)scaled&~1u;
    } else {
        p->height=p->canvas_height;
        scaled=(uint64_t)p->height*dar_n/dar_d;
        p->width=(uint32_t)scaled&~1u;
    }
    if(p->width<4u || p->height<4u) return -ENOTSUP;
    p->x=(p->canvas_width-p->width)/2u; p->y=(p->canvas_height-p->height)/2u;
    p->scale_enabled=p->width!=f->visible_width || p->height!=f->visible_height;
    if(p->scale_enabled) {
        p->coef_x=512u*f->visible_width/p->width;
        p->coef_y=512u*f->visible_height/p->height;
    }
    p->y_offset=f->crop_top*p->memory.stride+f->crop_left;
    p->uv_offset=p->memory.uv_offset+(f->crop_top/2u)*p->memory.stride+f->crop_left;
    if(f->matrix_coefficients==1u) p->matrix=1u;
    else if(f->matrix_coefficients==5u || f->matrix_coefficients==6u) p->matrix=6u;
    else if(f->matrix_coefficients==2u) {
        p->matrix=f->visible_width>=1280u || f->visible_height>=720u?1u:6u;
        p->matrix_inferred=1u;
    } else return -ENOTSUP;
    p->full_range=f->full_range==1;
    p->range_inferred=f->full_range<0;
    if(p->matrix==6u && p->full_range) { y=0x400; rv=0x59c; gu=0x160; gv=0x2db; bu=0x717; }
    else if(p->matrix==6u) { y=0x4a8; rv=0x662; gu=0x191; gv=0x340; bu=0x812; }
    else if(p->full_range) { y=0x400; rv=0x64d; gu=0xc0; gv=0x1df; bu=0x76c; }
    else { y=0x4a8; rv=0x72c; gu=0xda; gv=0x222; bu=0x873; }
    p->mult_yrv=y|(rv<<16); p->mult_gugv=gu|(gv<<16); p->mult_bu=bu;
    p->sub_yuv=(0x80u<<16)|(p->full_range?0u:16u);
    return 0;
}

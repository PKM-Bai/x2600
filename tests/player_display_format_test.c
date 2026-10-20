#include "player_display_format.h"
#include <assert.h>
#include <errno.h>
#include <stdio.h>

static void rgb(const player_display_format_plan_t *p,int yy,int u,int v,int *r,int *g,int *b)
{
    int y=yy-(int)(p->sub_yuv&31u), cb=u-128, cr=v-128;
    int cy=(int)(p->mult_yrv&2047u), rv=(int)(p->mult_yrv>>16);
    int gu=(int)(p->mult_gugv&511u),gv=(int)(p->mult_gugv>>16),bu=(int)p->mult_bu;
    *r=(cy*y+rv*cr+512)>>10; *g=(cy*y-gu*cb-gv*cr+512)>>10; *b=(cy*y+bu*cb+512)>>10;
    if(*r<0)*r=0; if(*r>255)*r=255;
    if(*g<0)*g=0; if(*g>255)*g=255;
    if(*b<0)*b=0; if(*b>255)*b=255;
}
int main(void)
{
    const uint32_t cases[][9]={
        {320,240,320,240,384,960,720,160,0},
        {640,368,640,360,640,1280,720,0,0},
        {640,480,640,480,640,960,720,160,0},
        {864,480,854,480,896,1280,718,0,1},
        {960,544,960,540,1024,1280,720,0,0},
        {1280,720,1280,720,1280,1280,720,0,0},
        {720,1280,720,1280,768,720,1280,0,0}
    };
    player_video_format_t f={640,480,640,480,0,0,1,1,6,1,2,2};
    player_display_format_plan_t p;
    for(unsigned i=0;i<sizeof(cases)/sizeof(*cases);i++) {
        const uint32_t *c=cases[i];
        f.coded_width=c[0]; f.coded_height=c[1]; f.visible_width=c[2]; f.visible_height=c[3];
        assert(!player_display_plan_format(&f,&p));
        assert(p.memory.stride==c[4] && p.width==c[5] && p.height==c[6] && p.x==c[7] && p.y==c[8]);
        assert(p.memory.uv_offset==c[4]*c[1]);
        assert(p.memory.total_bytes==c[4]*c[1]*3u/2u);
        assert(p.canvas_width==(i==6?720u:1280u));
        assert(p.x+p.width<=p.canvas_width && p.y+p.height<=p.canvas_height);
    }
    f=(player_video_format_t){720,576,720,576,0,0,16,15,6,0,2,2};
    assert(!player_display_plan_format(&f,&p) && p.width==960 && p.height==720 && p.x==160);
    f=(player_video_format_t){656,368,640,360,8,2,1,1,6,1,2,2};
    assert(!player_display_plan_format(&f,&p));
    assert(p.y_offset==2u*768u+8u && p.uv_offset==368u*768u+768u+8u);
    f.crop_left=2; assert(player_display_plan_format(&f,&p)==-ENOTSUP);
    f.crop_left=8; f.crop_top=1; assert(player_display_plan_format(&f,&p)==-ENOTSUP);
    f.crop_top=2; f.visible_width=650; assert(player_display_plan_format(&f,&p)==-ENOTSUP);
    f=(player_video_format_t){1920,1088,1920,1080,0,0,1,1,1,0,2,2};
    assert(player_display_plan_format(&f,&p)==-ENOTSUP);
    f=(player_video_format_t){640,368,640,360,0,0,0,0,2,-1,2,2};
    assert(!player_display_plan_format(&f,&p) && p.matrix==6 && p.matrix_inferred && p.range_inferred && !p.full_range);
    f.sar_num=1; assert(player_display_plan_format(&f,&p)==-EINVAL);
    f.sar_den=1; f.matrix_coefficients=9; assert(player_display_plan_format(&f,&p)==-ENOTSUP);
    f.matrix_coefficients=6; f.full_range=2; assert(player_display_plan_format(&f,&p)==-EINVAL);
    const uint32_t csc[][5]={
        {6,1,0x059c0400,0x02db0160,0x717}, {6,0,0x066204a8,0x03400191,0x812},
        {1,1,0x064d0400,0x01df00c0,0x76c}, {1,0,0x072c04a8,0x022200da,0x873}
    };
    for(unsigned i=0;i<4;i++) {
        f=(player_video_format_t){1280,720,1280,720,0,0,1,1,csc[i][0],(int32_t)csc[i][1],2,2};
        assert(!player_display_plan_format(&f,&p));
        assert(p.mult_yrv==csc[i][2] && p.mult_gugv==csc[i][3] && p.mult_bu==csc[i][4]);
        assert(p.sub_yuv==(csc[i][1]?0x00800000u:0x00800010u));
        int r,g,b;
        rgb(&p,csc[i][1]?0:16,128,128,&r,&g,&b); assert(r==0 && g==0 && b==0);
        rgb(&p,csc[i][1]?255:235,128,128,&r,&g,&b); assert(r==255 && g==255 && b==255);
        if(!csc[i][1]) {
            rgb(&p,csc[i][0]==6?81:63,csc[i][0]==6?90:102,240,&r,&g,&b);
            assert(r>=254 && g<=1 && b==0);
        }
    }
    puts("PASS step4 format: coded padding/common sizes/SAR/crop/720p bound/CSC black-white-red/range fallback");
    return 0;
}

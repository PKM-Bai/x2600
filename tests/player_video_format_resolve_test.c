#include "player_video.h"
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>

int main(void)
{
    player_video_format_t s={640,368,640,360,0,0,1,1,1,0,1,1};
    player_video_format_t c={0,0,0,0,0,0,16,15,6,1,6,6}, out;
    assert(!player_video_format_resolve(&s,&c,&out));
    assert(!memcmp(&out,&s,sizeof(s))); /* All known SPS properties win. */
    s.full_range=-1; s.sar_num=s.sar_den=0; s.colour_primaries=2;
    c.transfer_characteristics=16; /* Container PQ does not replace known SPS SDR. */
    assert(!player_video_format_resolve(&s,&c,&out));
    assert(out.matrix_coefficients==1 && out.full_range==1 && out.sar_num==16 && out.sar_den==15);
    assert(out.colour_primaries==6 && out.transfer_characteristics==1);
    assert(out.coded_width==640 && out.coded_height==368 && out.visible_height==360);
    s=(player_video_format_t){640,368,640,360,0,0,1,1,2,0,2,2};
    c.transfer_characteristics=6;
    assert(!player_video_format_resolve(&s,&c,&out));
    assert(out.matrix_coefficients==6 && out.full_range==0 && out.sar_num==1 && out.sar_den==1);
    assert(out.colour_primaries==6 && out.transfer_characteristics==6);
    s=(player_video_format_t){640,368,640,360,0,0,0,0,2,-1,2,2};
    assert(!player_video_format_resolve(&s,NULL,&out) && !memcmp(&s,&out,sizeof(s)));
    c.matrix_coefficients=9; /* Explicit BT2020 cannot become inferred HD709. */
    assert(player_video_format_resolve(&s,&c,&out)==-ENOTSUP);
    c.matrix_coefficients=6; c.transfer_characteristics=16;
    assert(player_video_format_resolve(&s,&c,&out)==-ENOTSUP);
    c.transfer_characteristics=18;
    assert(player_video_format_resolve(&s,&c,&out)==-ENOTSUP);
    c.transfer_characteristics=6; c.colour_primaries=9;
    assert(player_video_format_resolve(&s,&c,&out)==-ENOTSUP);
    c.colour_primaries=6;
    s.matrix_coefficients=9;
    assert(player_video_format_resolve(&s,&c,&out)==-ENOTSUP); /* Container does not repair explicit SPS2020. */
    s=(player_video_format_t){640,368,640,360,0,0,1,1,1,0,1,1};
    c.matrix_coefficients=9; c.transfer_characteristics=16; c.colour_primaries=9;
    assert(!player_video_format_resolve(&s,&c,&out) && !memcmp(&s,&out,sizeof(s)));
    s.sar_num=0;
    assert(player_video_format_resolve(&s,&c,&out)==-EINVAL);
    assert(player_video_format_resolve(NULL,&c,&out)==-EINVAL);
    assert(player_video_format_resolve(&s,&c,NULL)==-EINVAL);
    puts("PASS video format resolve: per-property SPS precedence/container fallback/unknown retained/BT2020-PQ-HLG rejection");
    return 0;
}

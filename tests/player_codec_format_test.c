#define _CRT_SECURE_NO_WARNINGS
#include "player_codec_format.h"
#include <assert.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdarg.h>

void av_log(void *context,int level,const char *format,...)
{ (void)context;(void)level;(void)format; }
#ifdef _MSC_VER
int __builtin_ctz(unsigned value)
{ int n=0;assert(value);while(!(value&1u)){value>>=1;++n;}return n; }
int __builtin_clz(unsigned value)
{ int n=0;assert(value);while(!(value&0x80000000u)){value<<=1;++n;}return n; }
#endif

static void unit_cases(void)
{
    SPS sps={0}; player_video_format_t f;
    sps.mb_width=40; sps.mb_height=23; sps.crop_bottom=8;
    sps.video_signal_type_present_flag=1; sps.colour_description_present_flag=1;
    sps.matrix_coefficients=1; sps.colour_primaries=1; sps.full_range=1; sps.transfer_characteristics=16;
    /* No VUI means those stale/raw values cannot become effective metadata. */
    assert(!player_codec_format_from_sps(&sps,&f));
    assert(f.coded_width==640 && f.coded_height==368 && f.visible_width==640 && f.visible_height==360);
    assert(f.full_range==-1 && f.matrix_coefficients==2 && !f.sar_num && !f.sar_den);
    sps.vui_parameters_present_flag=1;
    assert(player_codec_format_from_sps(&sps,&f)==-ENOTSUP); /* PQ */
    sps.transfer_characteristics=18; assert(player_codec_format_from_sps(&sps,&f)==-ENOTSUP);
    sps.transfer_characteristics=1;
    assert(!player_codec_format_from_sps(&sps,&f) && f.full_range==1 && f.matrix_coefficients==1);
    sps.full_range=0;
    assert(!player_codec_format_from_sps(&sps,&f) && f.full_range==0); /* Explicit limited is known. */
    sps.colour_description_present_flag=0;
    assert(!player_codec_format_from_sps(&sps,&f) && f.full_range==0 && f.matrix_coefficients==2);
    sps.video_signal_type_present_flag=0;
    assert(!player_codec_format_from_sps(&sps,&f) && f.full_range==-1 && f.matrix_coefficients==2);
    sps.sar.num=16; sps.sar.den=15;
    assert(!player_codec_format_from_sps(&sps,&f) && f.sar_num==16 && f.sar_den==15);
    sps.sar.den=0; assert(player_codec_format_from_sps(&sps,&f)==-ENOTSUP);
    sps.sar.num=sps.sar.den=0;
    sps.sar.den=1;
    assert(!player_codec_format_from_sps(&sps,&f) && !f.sar_num && !f.sar_den);
    sps.sar.den=0;
    sps.crop_bottom=368; assert(player_codec_format_from_sps(&sps,&f)==-ENOTSUP);
    sps.crop_bottom=8; sps.crop_left=8; sps.crop_right=8;
    assert(!player_codec_format_from_sps(&sps,&f) && f.visible_width==624 && f.crop_left==8);
    sps.mb_width=120; sps.mb_height=68; assert(player_codec_format_from_sps(&sps,&f)==-ENOTSUP);
    puts("PASS codec format unit: coded crop/SAR/absent VUI/explicit limited/HDR rejection/bounds");
}

int main(int argc,char **argv)
{
    static H264ParamSets parameters;
    AVCodecContext avctx={0};
    unit_cases();
    /* Reuse SPS id0 across all inputs; omitted color must clear old metadata. */
    for(int index=1;index<argc;index++) {
        uint8_t bytes[4096+64]={0};
        FILE *input=fopen(argv[index],"rb");assert(input);
        size_t count=fread(bytes,1,4096,input);assert(count>0 && !ferror(input));fclose(input);
        GetBitContext bits; assert(!init_get_bits(&bits,bytes,(int)count*8));
        assert(!ff_h264_decode_seq_parameter_set(&bits,&avctx,&parameters,0));
        const SPS *sps=&parameters.sps_list[0]; player_video_format_t f;
        int mapped=player_codec_format_from_sps(sps,&f);
        if(mapped) fprintf(stderr,"map=%d file=%s mb=%dx%d crop=%u,%u,%u,%u sar=%d/%d vui=%d video=%d full=%d matrix=%d transfer=%d\n",
                           mapped,argv[index],sps->mb_width,sps->mb_height,sps->crop_left,sps->crop_top,sps->crop_right,
                           sps->crop_bottom,sps->sar.num,sps->sar.den,sps->vui_parameters_present_flag,
                           sps->video_signal_type_present_flag,sps->full_range,sps->matrix_coefficients,sps->transfer_characteristics);
        assert(!mapped);
        printf("{\"coded_width\":%u,\"coded_height\":%u,\"visible_width\":%u,\"visible_height\":%u,\"crop_left\":%u,\"crop_top\":%u,\"sar_num\":%u,\"sar_den\":%u,\"matrix_coefficients\":%u,\"full_range\":%" PRId32 ",\"colour_primaries\":%d,\"transfer_characteristics\":%d,\"video_signal\":%d,\"colour_description\":%d,\"crop_right\":%u,\"crop_bottom\":%u,\"profile_idc\":%d,\"level_idc\":%d}\n",
               f.coded_width,f.coded_height,f.visible_width,f.visible_height,f.crop_left,f.crop_top,f.sar_num,f.sar_den,
               f.matrix_coefficients,f.full_range,sps->colour_primaries,sps->transfer_characteristics,
               sps->video_signal_type_present_flag,sps->colour_description_present_flag,
               sps->crop_right,sps->crop_bottom,sps->profile_idc,sps->level_idc);
        fflush(stdout);
    }
    return 0;
}

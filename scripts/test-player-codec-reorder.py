#!/usr/bin/env python3
"""Exercise the derived SDK's actual POC output and EOS-drain functions."""
from pathlib import Path
import argparse
import importlib.util
import os
import subprocess
import shutil

ROOT = Path(__file__).resolve().parents[1]

def load(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module

def function(text, name):
    start = text.index(name + '(')
    start = text.rfind('\n', 0, start) + 1
    opening = text.index('{', start)
    level = 1
    end = opening + 1
    while level:
        if text[end] == '{': level += 1
        elif text[end] == '}': level -= 1
        end += 1
    return text[start:end]

HEADER = r'''
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include "avassert.h"
#include "h264dec.h"
#include "mpegutils.h"
#undef container_of
#define container_of(ptr,type,member) ((type *)((char *)(ptr)-offsetof(type,member)))
static unsigned publish_calls;
typedef struct {
    uint32_t weight_approximated_frames;
    int submitting_weight_approximation;
    uint64_t submitting_frame;
} player_codec_t;
void av_log(void *ctx,int level,const char *fmt,...) {
    (void)ctx;(void)fmt;
    if(level==AV_LOG_PANIC) { fputs("FAIL SDK assertion\n",stderr);exit(1); }
}
int player_codec_frame_publish(H264Context *h,AVFrame *f) {
    unsigned *published=f->opaque;
    (void)h;
    if((*published)++) { fputs("FAIL duplicate frame publication\n",stderr);return -1; }
    ++publish_calls;return 0;
}
'''

TEST = r'''
static int collect(H264Context *h,unsigned *count,unsigned total) {
    AVFrame *f;
    while((f=h264_dequeue_frame(h))) {
        int64_t expected=(int64_t)(*count)*33333+(*count%3);
        if(*count>=total || f->pts!=expected) {
            fprintf(stderr,"FAIL output=%u pts=%lld expected=%lld\n",*count,(long long)f->pts,(long long)expected);
            return 1;
        }
        ++*count;
    }
    return 0;
}
static int run(const int *pocs,unsigned n,unsigned delay,unsigned gop) {
    H264Context h={0};AVCodecContext av={0};SPS sps={0};
    H264Picture pictures[32]={0};AVFrame frames[32]={0};unsigned published[32]={0};
    unsigned output=0,previous_calls=publish_calls;
    h.avctx=&av;h.ps.sps=&sps;h.next_outputed_poc=INT_MIN;
    sps.bitstream_restriction_flag=1;sps.num_reorder_frames=(int)delay;
    INIT_LIST_HEAD(&h.done_list);
    for(unsigned i=0;i<MAX_DELAYED_PIC_COUNT;++i) h.last_pocs[i]=INT_MIN;
    for(unsigned i=0;i<n;++i) {
        unsigned ordinal=(gop ? i/gop*gop:0)+(unsigned)pocs[i]/2;
        pictures[i].f=&frames[i];pictures[i].poc=pocs[i];pictures[i].recovered=1;
        frames[i].pts=(int64_t)ordinal*33333+(ordinal%3);
        frames[i].key_frame=pocs[i]==0;frames[i].pict_type=pocs[i]==0?AV_PICTURE_TYPE_I:AV_PICTURE_TYPE_B;
        frames[i].opaque=&published[i];
        h.cur_pic_ptr=&pictures[i];h.next_output_pic=NULL;
        if(gop && i%gop==0) h.mmco_reset=1;
        if(h264_select_output_frame(&h)) return 1;
        int got=0;
        if(h.next_output_pic && finalize_frame(&h,NULL,h.next_output_pic,&got)) return 1;
        if(i<delay && got) {fputs("FAIL output before reorder window is full\n",stderr);return 1;}
        if(collect(&h,&output,n)) return 1;
    }
    unsigned flushed=0;
    for(unsigned i=0;i<n+1;++i) {
        int got=0;
        if(send_next_delayed_frame(&h,NULL,&got,0)<0 || collect(&h,&output,n)) return 1;
        if(!got) break;
        ++flushed;
    }
    if(output!=n || publish_calls-previous_calls!=n || flushed!=delay) {
        fprintf(stderr,"FAIL total=%u/%u published=%u tail=%u/%u\n",output,n,publish_calls-previous_calls,flushed,delay);
        return 1;
    }
    for(unsigned i=0;i<n;++i) if(published[i]!=1) return 1;
    printf("PASS POC reorder=%u count=%u EOS=%u gop=%u once-only real PTS\n",delay,n,flushed,gop);
    return 0;
}
int main(void) {
    static const int r1[]={0,6,2,4,12,8,10,14};
    static const int r2[]={0,10,6,2,4,8,20,16,12,14,18,22};
    static const int r4[]={0,18,10,6,2,4,8,14,12,16,28,24,20,22,26,30};
    static const int idr[]={0,10,6,2,4,8,0,10,6,2,4,8};
    return test_weights()||test_references()||run(r1,8,1,0)||run(r2,12,2,0)||run(r4,16,4,0)||run(idr,12,2,6);
}
'''

REFERENCE_TEST = r'''
static int test_references(void) {
    H264SliceContext slice={0};jzm_h264 hardware={0};
    H264Picture live={0},released={0};AVFrame frame={0};AVBuffer planes[4]={{0}};
    live.f=&frame;live.frm_info_ctrl=&planes[0];live.frm_info_mv=&planes[1];
    live.dec_result_y=&planes[2];live.dec_result_uv=&planes[3];
    for(unsigned i=0;i<4;++i) planes[i].buffer_pa=0x12340000u+i*0x10000u;
    for(unsigned list=0;list<2;++list)
        for(unsigned i=0;i<16;++i) slice.ref_list[list][i].parent=&released;
    /* The real crash was a P slice's inactive list1 entry pointing to an
     * already released picture. Also leave a stale ref_count1 from B. */
    slice.list_count=1;slice.ref_count[0]=3;slice.ref_count[1]=4;
    for(unsigned i=0;i<3;++i) slice.ref_list[0][i].parent=&live;
    if(player_set_reference_buffers(&slice,&hardware)) return 1;
    for(unsigned list=0;list<2;++list)
        for(unsigned i=0;i<16;++i) {
            unsigned active=list==0 && i<3;
            if(hardware.mc_ref_y[list][i]!=(active?planes[2].buffer_pa:0) ||
               hardware.mc_ref_c[list][i]!=(active?planes[3].buffer_pa:0)) return 1;
        }
    if(hardware.ref_frm_ctrl || hardware.ref_frm_mv) return 1;
    slice.list_count=2;slice.ref_count[1]=1;slice.ref_list[1][0].parent=&live;
    if(player_set_reference_buffers(&slice,&hardware) ||
       hardware.ref_frm_ctrl!=planes[0].buffer_pa || hardware.ref_frm_mv!=planes[1].buffer_pa) return 1;
    slice.list_count=0;
    if(player_set_reference_buffers(&slice,&hardware) || hardware.ref_frm_ctrl ||
       hardware.ref_frm_mv || hardware.mc_ref_y[0][0] || hardware.mc_ref_y[1][0]) return 1;
    slice.list_count=1;slice.ref_list[0][0].parent=&released;
    if(player_set_reference_buffers(&slice,&hardware)!=AVERROR_INVALIDDATA) return 1;
    slice.ref_list[0][0].parent=NULL;
    if(player_set_reference_buffers(&slice,&hardware)!=AVERROR_INVALIDDATA) return 1;
    slice.ref_list[0][0].parent=&live;live.dec_result_y=NULL;
    if(player_set_reference_buffers(&slice,&hardware)!=AVERROR_INVALIDDATA) return 1;
    live.dec_result_y=&planes[2];slice.ref_count[0]=17;
    if(player_set_reference_buffers(&slice,&hardware)!=AVERROR_INVALIDDATA) return 1;
    puts("PASS actual SDK reference setup: I/P/B active bounds, stale released entries ignored, invalid active references rejected");
    return 0;
}
'''

WEIGHT_TEST = r'''
static int test_weights(void) {
    H264Context h={0};H264SliceContext slice={0};jzm_h264 hardware={0};player_codec_t codec={0};
    h.player_owner=&codec;h.st_h264=&hardware;codec.submitting_frame=12;
    slice.list_count=2;slice.ref_count[0]=2;slice.ref_count[1]=1;
    hardware.use_weight=IS_WT1;hardware.use_weight_chroma=1;
    hardware.luma_weight[0][0]=128;
    hardware.luma_weight[0][1]=(unsigned)-128;
    hardware.chroma_weight[0][0][0]=112;hardware.chroma_offset[0][0][0]=16;
    hardware.chroma_weight[0][0][1]=111;hardware.chroma_offset[0][0][1]=17;
    hardware.chroma_weight[0][1][0]=hardware.chroma_weight[0][1][1]=128;
    hardware.chroma_weight[1][0][0]=(unsigned)-128;hardware.chroma_weight[1][0][1]=128;
    hardware.luma_weight[1][3]=128;hardware.chroma_weight[1][3][0]=128;
    player_codec_prepare_weights(&h,&slice);
    if(hardware.luma_weight[0][0]!=127 || hardware.luma_weight[0][1]!=(unsigned)-128 ||
       hardware.chroma_weight[0][0][0]!=112 || hardware.chroma_offset[0][0][0]!=16 ||
       hardware.chroma_weight[0][0][1]!=111 || hardware.chroma_offset[0][0][1]!=17 ||
       hardware.chroma_weight[1][0][0]!=(unsigned)-128 || hardware.chroma_weight[1][0][1]!=127 ||
       hardware.luma_weight[1][3]!=128 || hardware.chroma_weight[1][3][0]!=128 ||
       MCE_RLUT_WT(hardware.chroma_weight[0][1][1],0,hardware.chroma_weight[0][1][0],0)!=0x7f007f00u ||
       !codec.submitting_weight_approximation || player_codec_weight_approximation_count(&codec)) return 1;
    /* A second affected slice of the same AU must not increment twice. */
    hardware.luma_weight[0][0]=128;player_codec_prepare_weights(&h,&slice);
    if(player_codec_complete_submit(&codec,64,64) || player_codec_weight_approximation_count(&codec)!=1 ||
       player_codec_weight_approximation_count(NULL)) return 1;
    codec.submitting_weight_approximation=0;
    hardware.use_weight=IS_WT2;hardware.luma_weight[0][0]=128;
    player_codec_prepare_weights(&h,&slice);
    if(codec.submitting_weight_approximation || hardware.luma_weight[0][0]!=128 ||
       player_codec_complete_submit(&codec,64,64) || player_codec_weight_approximation_count(&codec)!=1) return 1;
    hardware.use_weight=IS_WT1;hardware.use_weight_chroma=0;hardware.luma_weight[0][0]=64;
    hardware.chroma_weight[0][0][0]=128;player_codec_prepare_weights(&h,&slice);
    if(codec.submitting_weight_approximation || hardware.chroma_weight[0][0][0]!=128) return 1;
    codec.submitting_weight_approximation=1;
    if(player_codec_complete_submit(&codec,-12,64)!=-12 ||
       player_codec_complete_submit(&codec,63,64)!=-EIO || player_codec_weight_approximation_count(&codec)!=1) return 1;
    puts("PASS actual weight adapter/counter: active +128 only, signed -128 unchanged, disabled channels, multi-slice and failed AU accounting");
    return 0;
}
'''

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--derived',type=Path,help='Already derived SDK to copy into the isolated test directory')
    parser.add_argument('--output',type=Path,default=ROOT/'build/player-codec-reorder-host')
    args=parser.parse_args()
    output=args.output.resolve()
    output.mkdir(parents=True,exist_ok=True)
    if args.derived:
        shutil.copytree(args.derived.resolve(),output/'derived',dirs_exist_ok=True)
    else:
        prepare=load('reorder_prepare',ROOT/'scripts/prepare-player-codec.py')
        prepare.prepare(output/'derived')
    # MSVC rejects the vendor's unused empty caption struct; no tested field
    # or algorithm depends on its size. Keep this adaptation in the host copy.
    sei_path=output/'derived/include/h264_sei.h'
    sei=sei_path.read_text(encoding='utf-8').replace('typedef struct H264SEIA53Caption {',
        'typedef struct H264SEIA53Caption {\n    int player_host_unused;')
    sei_path.write_text(sei,encoding='utf-8')
    slices = (output / 'derived/src/h264_slice.c').read_text(encoding='utf-8')
    codec = (output / 'derived/src/h264dec.c').read_text(encoding='utf-8')
    parts = [function(codec, name) for name in ['h264_dequeue_frame','output_frame','finalize_frame','send_next_delayed_frame']]
    reference_helper = function(slices, 'player_set_reference_buffers')
    adapter = (ROOT/'projects/halley7m-player/codec/player_codec.c').read_text(encoding='utf-8')
    weight_parts = [function(adapter, name) for name in ['player_codec_prepare_weights',
                    'player_codec_complete_submit', 'player_codec_weight_approximation_count']]
    environment=load('reorder_env',ROOT/'scripts/test-player-amp.py').msvc_environment() if os.name=='nt' else os.environ.copy()
    compiler=shutil.which('cl' if os.name=='nt' else os.environ.get('CC','gcc'),
                         path=environment.get('Path',environment.get('PATH')))
    if not compiler: raise RuntimeError('Host C compiler is unavailable')
    for old in (False, True):
        select = function(slices, 'h264_select_output_frame')
        anchor='!out_of_order && pics > h->avctx->has_b_frames'
        if select.count(anchor)!=1: raise RuntimeError('SDK reorder gate anchor changed')
        if old: select=select.replace(anchor,'!out_of_order')
        source=output/('old.c' if old else 'fixed.c')
        source.write_text(HEADER+'\n'+'\n'.join(parts)+'\n'+reference_helper+'\n'+
                          '\n'.join(weight_parts)+'\n'+WEIGHT_TEST+'\n'+
                          REFERENCE_TEST+'\n'+select+'\n'+TEST,encoding='utf-8')
        exe=source.with_suffix('.exe' if os.name=='nt' else '.out')
        if os.name=='nt':
            command=[compiler,'/nologo','/std:c11','/utf-8','/O2','/w',
                     '/I'+str(output/'derived'),'/I'+str(output/'derived/include'),str(source),'/Fe:'+str(exe)]
        else:
            command=[compiler,'-std=gnu11','-O2','-w','-I'+str(output/'derived'),
                     '-I'+str(output/'derived/include'),str(source),'-o',str(exe)]
        subprocess.run(command,cwd=output,env=environment,check=True)
        result=subprocess.run([str(exe)],capture_output=True)
        if old:
            assert result.returncode and b'output before reorder' in result.stderr, result
            print('PASS old SDK missing-gate regression reproduced:', result.stderr.decode().strip())
        else:
            if result.returncode: raise RuntimeError(result.stderr.decode())
            print(result.stdout.decode().strip())

if __name__=='__main__': main()

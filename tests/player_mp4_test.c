#define _FILE_OFFSET_BITS 64
#define _CRT_SECURE_NO_WARNINGS
#include "player_mp4.h"
#include <assert.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if !defined(_MSC_VER)
#include <sys/types.h>
#endif

struct file_io { FILE *file; uint64_t bytes; uint32_t reads, max_read, short_read; };
static int32_t file_read(void *opaque, void *out, uint32_t bytes)
{
    struct file_io *f=(struct file_io *)opaque;
    size_t n;
    if(f->short_read && bytes>7u) bytes=7u;
    n=fread(out,1,bytes,f->file);
    f->reads++; if(bytes>f->max_read) f->max_read=bytes;
    return ferror(f->file)?-1:(int32_t)n;
}
static int file_seek(void *opaque, uint64_t pos)
{
    struct file_io *f=(struct file_io *)opaque;
    if(pos>INT64_MAX) return -1;
#if defined(_MSC_VER)
    return _fseeki64(f->file,(int64_t)pos,SEEK_SET);
#else
    return fseeko(f->file,(off_t)pos,SEEK_SET);
#endif
}
static uint64_t file_size(void *opaque) { return ((struct file_io *)opaque)->bytes; }
static FILE *output_file(const char *prefix, const char *suffix)
{
    size_t length=strlen(prefix)+strlen(suffix)+1u;
    char *name=(char *)malloc(length); FILE *f;
    assert(name); snprintf(name,length,"%s%s",prefix,suffix); f=fopen(name,"wb"); free(name); assert(f); return f;
}
static void print_track(const char *name, const struct player_mp4_track_info *t)
{
    printf("\"%s\":{\"codec\":%u,\"timescale\":%u,\"samples\":%u,\"duration\":%" PRIu64 ",\"header_duration\":%" PRIu64 ",\"width\":%u,\"height\":%u,\"rate\":%u,\"channels\":%u,\"frame_samples\":%u,\"profile\":%u,\"level\":%u,\"nal_length\":%u,\"ctts\":%u,\"edit\":%u,\"media_start\":%" PRId64 ",\"empty_duration\":%" PRIu64 ",\"edit_duration\":%" PRIu64 ",\"config_bytes\":%u,\"presentation_start_us\":%" PRId64 ",\"presentation_end_us\":%" PRId64 ",\"presentation_last_pts_us\":%" PRId64 ",\"presentation_samples\":%u,\"matrix\":%u,\"primaries\":%u,\"transfer\":%u,\"full_range\":%d,\"sar_num\":%u,\"sar_den\":%u}",
           name,(unsigned)t->codec,t->timescale,t->sample_count,t->duration,t->header_duration,t->width,t->height,t->sample_rate,t->channels,t->aac_frame_samples,t->h264_profile,t->h264_level,t->nal_length_bytes,t->has_ctts,t->has_edit,t->edit_media_time,t->empty_edit_duration,t->edit_duration,t->config_bytes,t->presentation_start_us,t->presentation_end_us,t->presentation_last_pts_us,t->presentation_samples,t->matrix_coefficients,t->colour_primaries,t->transfer_characteristics,t->full_range,t->sar_num,t->sar_den);
}
int main(int argc, char **argv)
{
    struct file_io f; struct player_media_io io; struct player_mp4 *ctx=NULL; const struct player_mp4_info *info;
    struct player_mp4_packet p; FILE *video,*audio,*asc; uint8_t *buffer; uint32_t written,i,video_count=0,audio_count=0; int r;
    if(argc!=3 && argc!=4) {fprintf(stderr,"usage: player_mp4_test input output-prefix [short-read]\n"); return 3;}
    memset(&f,0,sizeof(f)); f.file=fopen(argv[1],"rb"); if(!f.file) return 3;
    f.short_read=argc==4;
#if defined(_MSC_VER)
    assert(!_fseeki64(f.file,0,SEEK_END)); f.bytes=(uint64_t)_ftelli64(f.file);
#else
    assert(!fseeko(f.file,0,SEEK_END)); f.bytes=(uint64_t)ftello(f.file);
#endif
    io.opaque=&f; io.read=file_read; io.seek=file_seek; io.size=file_size;
    r=player_mp4_open(&ctx,&io);
    if(r) {printf("{\"error\":%d,\"stage\":\"open\"}\n",r); fclose(f.file); return 2;}
    info=player_mp4_get_info(ctx); assert(info);
    printf("{\"info\":{\"movie_scale\":%u,\"movie_duration\":%" PRIu64 ",\"has_video\":%u,\"has_audio\":%u,",info->movie_timescale,info->movie_duration,info->has_video,info->has_audio);
    print_track("video",&info->video); printf(","); print_track("audio",&info->audio); printf("}}\n");
    video=output_file(argv[2],".h264"); audio=output_file(argv[2],".aacraw"); asc=output_file(argv[2],".asc");
    if(info->has_audio) assert(fwrite(info->audio.config,1,info->audio.config_bytes,asc)==info->audio.config_bytes);
    fclose(asc);
    buffer=(uint8_t *)malloc(PLAYER_MEDIA_VIDEO_MAX+PLAYER_MEDIA_INPUT_PADDING); assert(buffer);
    while((r=player_mp4_next(ctx,&p))==0) {
        memset(buffer,0xa5,PLAYER_MEDIA_VIDEO_MAX+PLAYER_MEDIA_INPUT_PADDING);
        written=99u;
        assert(player_mp4_read_packet(ctx,&p,buffer,p.normalized_bytes+PLAYER_MEDIA_INPUT_PADDING-1u,&written)==PLAYER_MEDIA_BUFFER_SMALL && !written);
        r=player_mp4_read_packet(ctx,&p,buffer,PLAYER_MEDIA_VIDEO_MAX+PLAYER_MEDIA_INPUT_PADDING,&written);
        if(r) break;
        assert(written==p.normalized_bytes);
        for(i=0;i<PLAYER_MEDIA_INPUT_PADDING;i++) assert(!buffer[written+i]);
        assert(fwrite(buffer,1,written,p.kind==PLAYER_PACKET_VIDEO?video:audio)==written);
        if(p.kind==PLAYER_PACKET_VIDEO) video_count++; else audio_count++;
        printf("{\"packet\":{\"kind\":%u,\"index\":%u,\"offset\":%" PRIu64 ",\"source\":%u,\"normalized\":%u,\"flags\":%u,\"dts\":%" PRId64 ",\"pts\":%" PRId64 ",\"scale\":%u,\"duration\":%u,\"dts_us\":%" PRId64 ",\"pts_us\":%" PRId64 ",\"duration_us\":%" PRIu64 "}}\n",
               (unsigned)p.kind,p.sample_index,p.source_offset,p.source_bytes,p.normalized_bytes,p.flags,p.dts,p.pts,p.timescale,p.duration,p.dts_us,p.pts_us,p.duration_us);
    }
    if(r==PLAYER_MEDIA_EOF) assert(player_mp4_next(ctx,&p)==PLAYER_MEDIA_EOF);
    fclose(video); fclose(audio); free(buffer); player_mp4_close(ctx); fclose(f.file);
    if(r!=PLAYER_MEDIA_EOF) {printf("{\"error\":%d,\"stage\":\"packet\"}\n",r); return 2;}
    printf("{\"done\":{\"video\":%u,\"audio\":%u,\"reads\":%u,\"max_read\":%u}}\n",video_count,audio_count,f.reads,f.max_read);
    return 0;
}

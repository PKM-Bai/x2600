#include "player_mp4.h"
#include <limits.h>
#include <stdlib.h>
#include <string.h>

#define FCC(a,b,c,d) (((uint32_t)(a)<<24)|((uint32_t)(b)<<16)|((uint32_t)(c)<<8)|(uint32_t)(d))
#define PAGE_BYTES 512u
#define TABLE_COUNT 6u
enum { T_SIZE, T_CHUNK, T_MAP, T_TIME, T_CTIME, T_SYNC };

struct box { uint64_t start, data, end; uint32_t type; };
struct table {
    uint64_t offset;
    uint32_t count, stride, page_first, page_entries;
    uint8_t page[PAGE_BYTES];
};
struct track {
    struct player_mp4_track_info info;
    struct table tables[TABLE_COUNT];
    uint8_t config[PLAYER_MP4_CONFIG_MAX];
    uint8_t headers[PLAYER_MP4_CONFIG_MAX];
    uint32_t headers_bytes, fixed_size, ctts_signed, present;
    uint32_t index, chunk, in_chunk, samples_per_chunk, map_index;
    uint64_t chunk_offset, chunk_used;
    uint32_t time_index, time_left, sample_duration;
    uint32_t ctime_index, ctime_left, sync_index;
    int64_t composition_offset, decode_time;
    struct player_mp4_packet peek;
    uint32_t peek_valid;
};
struct player_mp4 {
    struct player_media_io io;
    uint64_t bytes;
    struct player_mp4_info info;
    struct track tracks[2];
    uint64_t mdat_start, mdat_end;
};

static uint16_t be16(const uint8_t *p) { return (uint16_t)(((uint16_t)p[0]<<8)|p[1]); }
static uint32_t be32(const uint8_t *p) { return ((uint32_t)p[0]<<24)|((uint32_t)p[1]<<16)|((uint32_t)p[2]<<8)|p[3]; }
static uint64_t be64(const uint8_t *p) { return ((uint64_t)be32(p)<<32)|be32(p+4); }
static int read_at(struct player_mp4 *m, uint64_t pos, void *buffer, uint32_t bytes)
{
    uint8_t *p = (uint8_t *)buffer;
    uint32_t done = 0;
    if (pos > m->bytes || bytes > m->bytes-pos) return PLAYER_MEDIA_BAD_FORMAT;
    if (m->io.seek(m->io.opaque, pos)) return PLAYER_MEDIA_IO_ERROR;
    while (done < bytes) {
        int32_t n = m->io.read(m->io.opaque, p+done, bytes-done);
        if (n <= 0 || (uint32_t)n > bytes-done) return PLAYER_MEDIA_IO_ERROR;
        done += (uint32_t)n;
    }
    return PLAYER_MEDIA_OK;
}
static int get_box(struct player_mp4 *m, uint64_t pos, uint64_t end, struct box *b)
{
    uint8_t h[16]; uint64_t length; uint32_t header = 8u; int r;
    if (pos == end) return PLAYER_MEDIA_EOF;
    if (pos > end || end-pos < 8u) return PLAYER_MEDIA_BAD_FORMAT;
    r = read_at(m, pos, h, 8); if (r) return r;
    length = be32(h); b->type = be32(h+4);
    if (length == 1u) {
        if (end-pos < 16u) return PLAYER_MEDIA_BAD_FORMAT;
        r = read_at(m, pos+8u, h+8, 8); if (r) return r;
        length = be64(h+8); header = 16u;
    } else if (!length) length = end-pos;
    if (length < header || length > end-pos) return PLAYER_MEDIA_BAD_FORMAT;
    b->start=pos; b->data=pos+header; b->end=pos+length;
    return PLAYER_MEDIA_OK;
}
static int find_box(struct player_mp4 *m, uint64_t pos, uint64_t end, uint32_t type, struct box *out)
{
    struct box b; int r;
    while (pos < end) {
        r=get_box(m,pos,end,&b); if(r) return r;
        if (b.type==type) { *out=b; return PLAYER_MEDIA_OK; }
        pos=b.end;
    }
    return PLAYER_MEDIA_EOF;
}
static int payload(struct player_mp4 *m, const struct box *b, uint8_t *p, uint32_t bytes)
{
    if (bytes>b->end-b->data) return PLAYER_MEDIA_BAD_FORMAT;
    return read_at(m,b->data,p,bytes);
}
static int table_init(struct player_mp4 *m, struct table *t, const struct box *b, uint32_t stride, uint32_t skip)
{
    uint8_t h[8]; int r;
    if(t->stride) return PLAYER_MEDIA_BAD_FORMAT;
    r=payload(m,b,h,8); if(r) return r;
    if(h[0]>1u || h[1] || h[2] || h[3] || (h[0] && b->type!=FCC('c','t','t','s'))) return PLAYER_MEDIA_UNSUPPORTED;
    t->offset=b->data+skip; t->count=be32(h+4); t->stride=stride;
    if (skip>b->end-b->data || (uint64_t)t->count*stride>b->end-t->offset) return PLAYER_MEDIA_BAD_FORMAT;
    return PLAYER_MEDIA_OK;
}
static int entry(struct player_mp4 *m, struct table *t, uint32_t index, const uint8_t **out)
{
    uint32_t n,first; int r;
    if(!t->stride || index>=t->count) return PLAYER_MEDIA_BAD_FORMAT;
    if(!t->page_entries || index<t->page_first || index-t->page_first>=t->page_entries) {
        n=PAGE_BYTES/t->stride; first=(index/n)*n;
        if(n>t->count-first) n=t->count-first;
        r=read_at(m,t->offset+(uint64_t)first*t->stride,t->page,n*t->stride); if(r) return r;
        t->page_first=first; t->page_entries=n;
    }
    *out=t->page+(index-t->page_first)*t->stride;
    return PLAYER_MEDIA_OK;
}
static int get_u32(struct player_mp4 *m, struct table *t, uint32_t index, uint32_t *out)
{
    const uint8_t *p; int r=entry(m,t,index,&p); if(r) return r; *out=be32(p); return 0;
}
/* Absolute rescale, so rounding never accumulates across a long VFR track. */
static int rescale_us(int64_t value, uint32_t scale, int64_t *out)
{
    int64_t whole, fraction;
    if(!scale) return PLAYER_MEDIA_BAD_FORMAT;
    whole=value/(int64_t)scale; fraction=value%(int64_t)scale;
    if(whole>INT64_MAX/1000000 || whole<INT64_MIN/1000000) return PLAYER_MEDIA_UNSUPPORTED;
    whole*=1000000; fraction=(fraction*1000000)/(int64_t)scale;
    if((fraction>0 && whole>INT64_MAX-fraction) ||
       (fraction<0 && whole<INT64_MIN-fraction)) return PLAYER_MEDIA_UNSUPPORTED;
    *out=whole+fraction; return 0;
}
static int map_us(struct player_mp4 *m, struct track *t, int64_t source, int64_t *out)
{
    int64_t start=0, mapped; int r;
    if(t->info.has_edit) {
        if(source<INT64_MIN+t->info.edit_media_time) return PLAYER_MEDIA_BAD_FORMAT;
        source-=t->info.edit_media_time;
        if(t->info.empty_edit_duration>INT64_MAX) return PLAYER_MEDIA_UNSUPPORTED;
        r=rescale_us((int64_t)t->info.empty_edit_duration,m->info.movie_timescale,&start); if(r) return r;
    }
    r=rescale_us(source,t->info.timescale,&mapped); if(r) return r;
    if(mapped>INT64_MAX-start) return PLAYER_MEDIA_UNSUPPORTED;
    *out=mapped+start; return 0;
}
static int parse_avcc(struct track *t, const uint8_t *p, uint32_t bytes)
{
    uint32_t pos=6u, group, count, i, n, out=0;
    if(bytes<7u || p[0]!=1u) return PLAYER_MEDIA_BAD_FORMAT;
    t->info.h264_profile=p[1]; t->info.h264_level=p[3];
    t->info.nal_length_bytes=(p[4]&3u)+1u;
    if(t->info.nal_length_bytes==3u) return PLAYER_MEDIA_UNSUPPORTED;
    count=p[5]&31u;
    if(!count) return PLAYER_MEDIA_BAD_FORMAT;
    for(group=0;group<2u;group++) {
        for(i=0;i<count;i++) {
            if(bytes-pos<2u) return PLAYER_MEDIA_BAD_FORMAT;
            n=be16(p+pos); pos+=2u;
            if(!n || n>bytes-pos || out>PLAYER_MP4_CONFIG_MAX-4u || n>PLAYER_MP4_CONFIG_MAX-4u-out) return PLAYER_MEDIA_BAD_FORMAT;
            if((p[pos]&31u)!=(group?8u:7u)) return PLAYER_MEDIA_BAD_FORMAT;
            t->headers[out++]=0; t->headers[out++]=0; t->headers[out++]=0; t->headers[out++]=1;
            memcpy(t->headers+out,p+pos,n); out+=n; pos+=n;
        }
        if(!group) { if(pos>=bytes) return PLAYER_MEDIA_BAD_FORMAT; count=p[pos++]; if(!count) return PLAYER_MEDIA_BAD_FORMAT; }
    }
    t->headers_bytes=out;
    return 0;
}
struct bits { const uint8_t *p; uint32_t bytes,bit; };
static int bits_get(struct bits *b, uint32_t count, uint32_t *out)
{
    uint32_t i,v=0;
    if(count>24u || b->bit>(uint64_t)b->bytes*8u || count>(uint64_t)b->bytes*8u-b->bit) return PLAYER_MEDIA_BAD_FORMAT;
    for(i=0;i<count;i++,b->bit++) v=(v<<1)|((b->p[b->bit/8u]>>(7u-b->bit%8u))&1u);
    *out=v; return 0;
}
static int parse_asc(struct track *t, const uint8_t *p, uint32_t bytes)
{
    static const uint32_t rates[13]={96000,88200,64000,48000,44100,32000,24000,22050,16000,12000,11025,8000,7350};
    struct bits b; uint32_t object,index,rate,channels,frame,depends,extension; int r;
    b.p=p; b.bytes=bytes; b.bit=0;
    r=bits_get(&b,5,&object); if(r) return r;
    if(object!=2u) return PLAYER_MEDIA_UNSUPPORTED;
    r=bits_get(&b,4,&index); if(r) return r;
    if(index==15u) {r=bits_get(&b,24,&rate); if(r) return r;} else {if(index>=13u) return PLAYER_MEDIA_BAD_FORMAT; rate=rates[index];}
    r=bits_get(&b,4,&channels); if(r) return r;
    if(!rate || !channels || channels>2u) return PLAYER_MEDIA_UNSUPPORTED;
    r=bits_get(&b,1,&frame); if(r) return r;
    r=bits_get(&b,1,&depends); if(r) return r;
    if(depends) return PLAYER_MEDIA_UNSUPPORTED;
    r=bits_get(&b,1,&extension); if(r) return r;
    if(extension) return PLAYER_MEDIA_UNSUPPORTED;
    /* An LC outer object type may still signal SBR in a sync extension. */
    if((uint64_t)bytes*8u-b.bit>=16u) {
        uint32_t sync,ext_object,sbr;
        r=bits_get(&b,11,&sync); if(r) return r;
        if(sync==0x2b7u) {
            r=bits_get(&b,5,&ext_object); if(r) return r;
            if(ext_object!=5u) return PLAYER_MEDIA_UNSUPPORTED;
            r=bits_get(&b,1,&sbr); if(r) return r;
            if(sbr) return PLAYER_MEDIA_UNSUPPORTED;
        }
    }
    t->info.sample_rate=rate; t->info.channels=channels; t->info.aac_frame_samples=frame?960u:1024u;
    return 0;
}
static int descriptor(const uint8_t *p, uint32_t bytes, uint32_t *pos, uint32_t *tag, uint32_t *length)
{
    uint32_t i,n=0,c;
    if(*pos>=bytes) return PLAYER_MEDIA_BAD_FORMAT;
    *tag=p[(*pos)++];
    for(i=0;i<4u;i++) {
        if(*pos>=bytes) return PLAYER_MEDIA_BAD_FORMAT;
        c=p[(*pos)++]; n=(n<<7)|(c&127u);
        if(!(c&128u)) {if(n>bytes-*pos) return PLAYER_MEDIA_BAD_FORMAT; *length=n; return 0;}
    }
    return PLAYER_MEDIA_BAD_FORMAT;
}
static int parse_esds(struct track *t, const uint8_t *p, uint32_t bytes)
{
    uint32_t pos=4,tag,n,end,flags,decoder_end; int r;
    if(bytes<4u || p[0]) return PLAYER_MEDIA_UNSUPPORTED;
    r=descriptor(p,bytes,&pos,&tag,&n); if(r) return r;
    if(tag!=3u || n<3u) return PLAYER_MEDIA_UNSUPPORTED;
    end=pos+n; flags=p[pos+2u]; pos+=3u;
    if(flags&128u) pos+=2u;
    if(flags&64u) {if(pos>=end) return PLAYER_MEDIA_BAD_FORMAT; pos+=1u+p[pos];}
    if(flags&32u) pos+=2u;
    if(pos>end) return PLAYER_MEDIA_BAD_FORMAT;
    r=descriptor(p,end,&pos,&tag,&n); if(r) return r;
    if(tag!=4u || n<13u) return PLAYER_MEDIA_BAD_FORMAT;
    decoder_end=pos+n;
    if(p[pos]!=0x40u || (p[pos+1u]>>2)!=5u) return PLAYER_MEDIA_UNSUPPORTED;
    pos+=13u;
    while(pos<decoder_end) {
        r=descriptor(p,decoder_end,&pos,&tag,&n); if(r) return r;
        if(tag==5u) {
            if(!n || n>PLAYER_MP4_CONFIG_MAX) return PLAYER_MEDIA_UNSUPPORTED;
            memmove(t->config,p+pos,n); t->info.config_bytes=n;
            return parse_asc(t,t->config,n);
        }
        pos+=n;
    }
    return PLAYER_MEDIA_BAD_FORMAT;
}
static int parse_stsd(struct player_mp4 *m, struct track *t, const struct box *b)
{
    struct box sample,child; uint8_t h[40]; uint64_t pos; uint32_t kind=t->info.codec; int r,found=0;
    r=payload(m,b,h,8); if(r) return r;
    if(be32(h) || be32(h+4)!=1u) return PLAYER_MEDIA_UNSUPPORTED;
    r=get_box(m,b->data+8,b->end,&sample); if(r) return r;
    if(sample.end!=b->end) return PLAYER_MEDIA_BAD_FORMAT;
    if(kind==PLAYER_MP4_H264) {
        if(sample.type!=FCC('a','v','c','1')) return PLAYER_MEDIA_UNSUPPORTED;
        r=payload(m,&sample,h,28); if(r) return r;
        if(be16(h+6)!=1u) return PLAYER_MEDIA_UNSUPPORTED;
        t->info.width=be16(h+24); t->info.height=be16(h+26);
        if(!t->info.width || !t->info.height || sample.end-sample.data<78u) return PLAYER_MEDIA_BAD_FORMAT;
        pos=sample.data+78u;
    } else {
        if(sample.type!=FCC('m','p','4','a')) return PLAYER_MEDIA_UNSUPPORTED;
        r=payload(m,&sample,h,28); if(r) return r;
        if(be16(h+6)!=1u || be16(h+8)) return PLAYER_MEDIA_UNSUPPORTED;
        pos=sample.data+28u;
    }
    while(pos<sample.end) {
        r=get_box(m,pos,sample.end,&child); if(r) return r;
        if(child.type==FCC('a','v','c','C') || child.type==FCC('e','s','d','s')) {
            if(found++ || child.end-child.data>PLAYER_MP4_CONFIG_MAX) return PLAYER_MEDIA_UNSUPPORTED;
            t->info.config_bytes=(uint32_t)(child.end-child.data);
            r=payload(m,&child,t->config,t->info.config_bytes); if(r) return r;
            if(kind==PLAYER_MP4_H264 && child.type==FCC('a','v','c','C')) r=parse_avcc(t,t->config,t->info.config_bytes);
            else if(kind==PLAYER_MP4_AAC_LC && child.type==FCC('e','s','d','s')) r=parse_esds(t,t->config,t->info.config_bytes);
            else r=PLAYER_MEDIA_BAD_FORMAT;
            if(r) return r;
        }
        if(child.type==FCC('s','i','n','f')) return PLAYER_MEDIA_UNSUPPORTED;
        pos=child.end;
    }
    t->info.config=t->config;
    return found?0:PLAYER_MEDIA_BAD_FORMAT;
}
static int parse_elst(struct player_mp4 *m, struct track *t, const struct box *edts)
{
    struct box b; uint8_t h[40]; uint32_t version,count,stride,i; uint64_t pos,duration; int64_t media; int r;
    r=find_box(m,edts->data,edts->end,FCC('e','l','s','t'),&b); if(r==PLAYER_MEDIA_EOF) return 0; if(r) return r;
    r=payload(m,&b,h,8); if(r) return r;
    version=h[0]; count=be32(h+4);
    if(version>1u || !count || count>2u) return PLAYER_MEDIA_UNSUPPORTED;
    stride=version?20u:12u;
    if((uint64_t)count*stride!=b.end-b.data-8u) return PLAYER_MEDIA_BAD_FORMAT;
    pos=b.data+8u;
    for(i=0;i<count;i++,pos+=stride) {
        r=read_at(m,pos,h,stride); if(r) return r;
        duration=version?be64(h):be32(h);
        media=version?(int64_t)be64(h+8):(int64_t)(int32_t)be32(h+4);
        if(be32(h+(version?16u:8u))!=0x00010000u) return PLAYER_MEDIA_UNSUPPORTED;
        if(!i && count==2u) {
            if(media!=-1) return PLAYER_MEDIA_UNSUPPORTED;
            t->info.empty_edit_duration=duration;
        } else {
            if(media<0 || !duration) return PLAYER_MEDIA_UNSUPPORTED;
            t->info.edit_media_time=media; t->info.edit_duration=duration;
        }
    }
    t->info.has_edit=1u;
    return 0;
}
static int parse_stbl(struct player_mp4 *m, struct track *t, const struct box *stbl)
{
    struct box b; uint64_t pos=stbl->data; uint8_t h[12]; int r; uint32_t seen_stsd=0;
    while(pos<stbl->end) {
        r=get_box(m,pos,stbl->end,&b); if(r) return r;
        switch(b.type) {
        case FCC('s','t','s','d'):
            if(seen_stsd++) return PLAYER_MEDIA_BAD_FORMAT;
            r=parse_stsd(m,t,&b); break;
        case FCC('s','t','s','z'):
            r=payload(m,&b,h,12); if(r) break;
            if(be32(h)) return PLAYER_MEDIA_UNSUPPORTED;
            if(t->tables[T_SIZE].stride) return PLAYER_MEDIA_BAD_FORMAT;
            t->fixed_size=be32(h+4); t->info.sample_count=be32(h+8);
            t->tables[T_SIZE].offset=b.data+12u; t->tables[T_SIZE].count=t->info.sample_count; t->tables[T_SIZE].stride=4u;
            if(!t->fixed_size && (uint64_t)t->info.sample_count*4u>b.end-b.data-12u) r=PLAYER_MEDIA_BAD_FORMAT;
            break;
        case FCC('s','t','c','o'): r=table_init(m,&t->tables[T_CHUNK],&b,4,8); break;
        case FCC('c','o','6','4'): r=table_init(m,&t->tables[T_CHUNK],&b,8,8); break;
        case FCC('s','t','s','c'): r=table_init(m,&t->tables[T_MAP],&b,12,8); break;
        case FCC('s','t','t','s'): r=table_init(m,&t->tables[T_TIME],&b,8,8); break;
        case FCC('c','t','t','s'):
            r=payload(m,&b,h,8); if(r) break;
            t->ctts_signed=h[0]; t->info.has_ctts=1;
            r=table_init(m,&t->tables[T_CTIME],&b,8,8); break;
        case FCC('s','t','s','s'): r=table_init(m,&t->tables[T_SYNC],&b,4,8); break;
        case FCC('s','t','z','2'): case FCC('s','e','n','c'): case FCC('s','a','i','z'):
            return PLAYER_MEDIA_UNSUPPORTED;
        default: r=0; break;
        }
        if(r) return r;
        pos=b.end;
    }
    if(!seen_stsd || !t->info.sample_count || !t->tables[T_SIZE].stride || !t->tables[T_CHUNK].count || !t->tables[T_MAP].count || !t->tables[T_TIME].count) return PLAYER_MEDIA_BAD_FORMAT;
    return 0;
}
/* Merge runs, rather than visit every frame. This validates presentation
 * clipping without consuming the playback cursor or reading coded samples. */
static int presentation_window(struct player_mp4 *m, struct track *t)
{
    uint32_t ti=0,ci=0,tl=0,cl=0,delta=0,count,first,after;
    uint64_t decoded=0,span,clip_end=UINT64_MAX;
    int64_t offset=0,clip_start=t->info.has_edit?t->info.edit_media_time:0;
    int64_t requested_end=0,natural_end=INT64_MIN,start=0,last=INT64_MIN;
    const uint8_t *p; int r;
    if(t->info.codec!=PLAYER_MP4_H264) return 0;
    if(t->info.has_edit) {
        uint64_t whole=t->info.edit_duration/m->info.movie_timescale;
        uint64_t fraction=t->info.edit_duration%m->info.movie_timescale;
        if(whole>((uint64_t)INT64_MAX-(uint64_t)clip_start)/t->info.timescale)
            return PLAYER_MEDIA_UNSUPPORTED;
        clip_end=(uint64_t)clip_start+whole*t->info.timescale;
        fraction*=t->info.timescale;
        uint64_t tail=fraction/m->info.movie_timescale+(fraction%m->info.movie_timescale!=0);
        if(tail>(uint64_t)INT64_MAX-clip_end) return PLAYER_MEDIA_UNSUPPORTED;
        clip_end+=tail; /* Ceil: a PTS immediately before a fractional end stays visible. */
        if(t->info.empty_edit_duration>INT64_MAX || t->info.edit_duration>INT64_MAX)
            return PLAYER_MEDIA_UNSUPPORTED;
        r=rescale_us((int64_t)t->info.empty_edit_duration,m->info.movie_timescale,&start); if(r) return r;
        r=rescale_us((int64_t)t->info.edit_duration,m->info.movie_timescale,&requested_end); if(r) return r;
        if(requested_end>INT64_MAX-start) return PLAYER_MEDIA_UNSUPPORTED;
        requested_end+=start;
    }
    while(decoded<t->info.duration) {
        if(!tl) {
            r=entry(m,&t->tables[T_TIME],ti++,&p); if(r) return r;
            tl=be32(p); delta=be32(p+4);
        }
        if(t->info.has_ctts && !cl) {
            r=entry(m,&t->tables[T_CTIME],ci++,&p); if(r) return r;
            cl=be32(p); offset=t->ctts_signed?(int64_t)(int32_t)be32(p+4):(int64_t)be32(p+4);
        }
        count=tl; if(t->info.has_ctts && count>cl) count=cl;
        span=(uint64_t)count*delta;
        if(offset>0 && decoded+span>(uint64_t)(INT64_MAX-offset)) return PLAYER_MEDIA_UNSUPPORTED;
        int64_t run=(int64_t)decoded+offset;
        int64_t end=run+(int64_t)span,mapped;
        r=map_us(m,t,end,&mapped); if(r) return r;
        if(mapped>natural_end) natural_end=mapped;
        first=0; after=count;
        if(run<clip_start) {
            uint64_t skip=((uint64_t)clip_start-(uint64_t)run)/delta;
            first=skip<count?(uint32_t)skip:count;
        }
        if(t->info.has_edit) {
            if(run>=(int64_t)clip_end) after=0;
            else {
                uint64_t room=clip_end-(uint64_t)run;
                uint64_t keep=room/delta+(room%delta!=0);
                if(keep<after) after=(uint32_t)keep;
            }
        }
        if(after>first) {
            int64_t pts=run+(int64_t)((uint64_t)(after-1u)*delta);
            r=map_us(m,t,pts,&mapped); if(r) return r;
            if(mapped>last) last=mapped;
            t->info.presentation_samples+=after-first;
        }
        decoded+=span; tl-=count; if(t->info.has_ctts) cl-=count;
    }
    t->info.presentation_start_us=start;
    t->info.presentation_end_us=natural_end;
    if(t->info.has_edit) {
        if(requested_end>natural_end) {
            uint64_t tick=(1000000u+(uint64_t)m->info.movie_timescale-1u)/m->info.movie_timescale;
            if((uint64_t)requested_end-(uint64_t)natural_end>tick) return PLAYER_MEDIA_BAD_FORMAT;
        } else t->info.presentation_end_us=requested_end;
    }
    t->info.presentation_last_pts_us=last;
    return 0;
}
static int validate_tables(struct player_mp4 *m, struct track *t)
{
    uint32_t i,count,delta,first,next,per,previous=0; uint64_t samples=0,duration=0,capacity=0; const uint8_t *p; int r;
    for(i=0;i<t->tables[T_TIME].count;i++) {
        r=entry(m,&t->tables[T_TIME],i,&p); if(r) return r;
        count=be32(p); delta=be32(p+4);
        if(!count || !delta) return PLAYER_MEDIA_BAD_FORMAT;
        samples+=count;
        if(samples>UINT32_MAX || (uint64_t)count*delta>(uint64_t)INT64_MAX-duration) return PLAYER_MEDIA_UNSUPPORTED;
        duration+=(uint64_t)count*delta;
    }
    if(samples!=t->info.sample_count) return PLAYER_MEDIA_BAD_FORMAT;
    t->info.duration=duration;
    samples=0;
    for(i=0;i<t->tables[T_CTIME].count;i++) {
        r=entry(m,&t->tables[T_CTIME],i,&p); if(r) return r;
        count=be32(p); if(!count) return PLAYER_MEDIA_BAD_FORMAT; samples+=count;
    }
    if(t->info.has_ctts && samples!=t->info.sample_count) return PLAYER_MEDIA_BAD_FORMAT;
    for(i=0;i<t->tables[T_MAP].count;i++) {
        r=entry(m,&t->tables[T_MAP],i,&p); if(r) return r;
        first=be32(p); per=be32(p+4);
        if(!first || (i?first<=previous:first!=1u) || !per || be32(p+8)!=1u || first>t->tables[T_CHUNK].count) return PLAYER_MEDIA_UNSUPPORTED;
        previous=first;
        if(i+1u<t->tables[T_MAP].count) {r=entry(m,&t->tables[T_MAP],i+1u,&p); if(r) return r; next=be32(p);} else next=t->tables[T_CHUNK].count+1u;
        if(next<=first || next-1u>t->tables[T_CHUNK].count) return PLAYER_MEDIA_BAD_FORMAT;
        capacity+=(uint64_t)(next-first)*per;
    }
    if(capacity!=t->info.sample_count) return PLAYER_MEDIA_BAD_FORMAT;
    previous=0;
    for(i=0;i<t->tables[T_SYNC].count;i++) {
        r=get_u32(m,&t->tables[T_SYNC],i,&count); if(r) return r;
        if(count<=previous || count>t->info.sample_count) return PLAYER_MEDIA_BAD_FORMAT;
        previous=count;
    }
    return presentation_window(m,t);
}
static int parse_trak(struct player_mp4 *m, const struct box *trak)
{
    struct box mdia,b,minf,stbl,edts; uint8_t h[40]; uint32_t kind; struct track *t; int r;
    r=find_box(m,trak->data,trak->end,FCC('m','d','i','a'),&mdia); if(r) return r<0?r:PLAYER_MEDIA_BAD_FORMAT;
    r=find_box(m,mdia.data,mdia.end,FCC('h','d','l','r'),&b); if(r) return r<0?r:PLAYER_MEDIA_BAD_FORMAT;
    r=payload(m,&b,h,12); if(r) return r;
    if(be32(h+8)==FCC('v','i','d','e')) kind=0;
    else if(be32(h+8)==FCC('s','o','u','n')) kind=1;
    else return 0;
    t=&m->tracks[kind]; if(t->present) return PLAYER_MEDIA_UNSUPPORTED;
    t->present=1; t->info.codec=kind?PLAYER_MP4_AAC_LC:PLAYER_MP4_H264;
    r=find_box(m,trak->data,trak->end,FCC('t','k','h','d'),&b); if(r) return r<0?r:PLAYER_MEDIA_BAD_FORMAT;
    r=payload(m,&b,h,24); if(r) return r;
    if(h[0]>1u) return PLAYER_MEDIA_UNSUPPORTED;
    t->info.track_id=be32(h+(h[0]?20u:12u));
    if(!t->info.track_id) return PLAYER_MEDIA_BAD_FORMAT;
    r=find_box(m,mdia.data,mdia.end,FCC('m','d','h','d'),&b); if(r) return r<0?r:PLAYER_MEDIA_BAD_FORMAT;
    if(b.end-b.data<20u) return PLAYER_MEDIA_BAD_FORMAT;
    r=payload(m,&b,h,32u<(b.end-b.data)?32u:(uint32_t)(b.end-b.data)); if(r) return r;
    if(h[0]>1u) return PLAYER_MEDIA_UNSUPPORTED;
    if(b.end-b.data<(h[0]?32u:20u)) return PLAYER_MEDIA_BAD_FORMAT;
    t->info.timescale=be32(h+(h[0]?20u:12u));
    t->info.header_duration=h[0]?be64(h+24):be32(h+16);
    if(!t->info.timescale) return PLAYER_MEDIA_BAD_FORMAT;
    r=find_box(m,mdia.data,mdia.end,FCC('m','i','n','f'),&minf); if(r) return r<0?r:PLAYER_MEDIA_BAD_FORMAT;
    {
        struct box dinf,dref,url;
        r=find_box(m,minf.data,minf.end,FCC('d','i','n','f'),&dinf);
        if(r==0) {
            r=find_box(m,dinf.data,dinf.end,FCC('d','r','e','f'),&dref); if(r) return r<0?r:PLAYER_MEDIA_BAD_FORMAT;
            r=payload(m,&dref,h,8); if(r) return r;
            if(be32(h) || be32(h+4)!=1u) return PLAYER_MEDIA_UNSUPPORTED;
            r=get_box(m,dref.data+8u,dref.end,&url); if(r) return r<0?r:PLAYER_MEDIA_BAD_FORMAT;
            r=payload(m,&url,h,4); if(r) return r;
            if(url.type!=FCC('u','r','l',' ') || be32(h)!=1u || url.end!=dref.end) return PLAYER_MEDIA_UNSUPPORTED;
        } else if(r<0) return r;
    }
    r=find_box(m,minf.data,minf.end,FCC('s','t','b','l'),&stbl); if(r) return r<0?r:PLAYER_MEDIA_BAD_FORMAT;
    r=parse_stbl(m,t,&stbl); if(r) return r;
    r=find_box(m,trak->data,trak->end,FCC('e','d','t','s'),&edts);
    if(r==0) {r=parse_elst(m,t,&edts); if(r) return r;} else if(r<0) return r;
    return validate_tables(m,t);
}
static int parse_moov(struct player_mp4 *m, const struct box *moov)
{
    struct box b; uint64_t pos=moov->data; uint8_t h[32]; int r; uint32_t mvhd=0;
    /* Movie timebase is required before interpreting edits. */
    r=find_box(m,moov->data,moov->end,FCC('m','v','h','d'),&b); if(r) return r<0?r:PLAYER_MEDIA_BAD_FORMAT;
    if(b.end-b.data<20u) return PLAYER_MEDIA_BAD_FORMAT;
    r=payload(m,&b,h,32u<(b.end-b.data)?32u:(uint32_t)(b.end-b.data)); if(r) return r;
    if(h[0]>1u) return PLAYER_MEDIA_UNSUPPORTED;
    if(b.end-b.data<(h[0]?32u:20u)) return PLAYER_MEDIA_BAD_FORMAT;
    m->info.movie_timescale=be32(h+(h[0]?20u:12u));
    m->info.movie_duration=h[0]?be64(h+24):be32(h+16);
    if(!m->info.movie_timescale) return PLAYER_MEDIA_BAD_FORMAT;
    while(pos<moov->end) {
        r=get_box(m,pos,moov->end,&b); if(r) return r;
        if(b.type==FCC('m','v','e','x')) return PLAYER_MEDIA_UNSUPPORTED;
        if(b.type==FCC('m','v','h','d')) mvhd++;
        if(b.type==FCC('t','r','a','k')) {r=parse_trak(m,&b); if(r) return r;}
        pos=b.end;
    }
    return mvhd==1u?0:PLAYER_MEDIA_BAD_FORMAT;
}
int player_mp4_open(struct player_mp4 **out, const struct player_media_io *io)
{
    struct player_mp4 *m; struct box b,moov; uint64_t pos=0; uint32_t found=0,mdat=0; int r=0;
    if(!out) return PLAYER_MEDIA_BAD_FORMAT;
    *out=NULL;
    if(!io || !io->read || !io->seek || !io->size) return PLAYER_MEDIA_BAD_FORMAT;
    m=(struct player_mp4 *)calloc(1,sizeof(*m)); if(!m) return PLAYER_MEDIA_BUFFER_SMALL;
    m->io=*io; m->bytes=io->size(io->opaque);
    memset(&moov,0,sizeof(moov));
    while(pos<m->bytes) {
        r=get_box(m,pos,m->bytes,&b); if(r) break;
        if(b.type==FCC('m','o','o','f')) {r=PLAYER_MEDIA_UNSUPPORTED; break;}
        if(b.type==FCC('m','o','o','v')) {if(found++) {r=PLAYER_MEDIA_BAD_FORMAT; break;} moov=b;}
        if(b.type==FCC('m','d','a','t')) {if(!mdat++) {m->mdat_start=b.data; m->mdat_end=b.end;}}
        pos=b.end;
    }
    if(!r && (!found || !mdat)) r=PLAYER_MEDIA_BAD_FORMAT;
    if(!r) r=parse_moov(m,&moov);
    if(!r && !m->tracks[0].present && !m->tracks[1].present) r=PLAYER_MEDIA_UNSUPPORTED;
    if(!r) {
        m->info.has_video=m->tracks[0].present; m->info.has_audio=m->tracks[1].present;
        m->info.video=m->tracks[0].info; m->info.audio=m->tracks[1].info;
        *out=m; return 0;
    }
    free(m); return r;
}
const struct player_mp4_info *player_mp4_get_info(const struct player_mp4 *ctx) { return ctx?&ctx->info:NULL; }
void player_mp4_close(struct player_mp4 *ctx) { free(ctx); }

static int sample_in_mdat(struct player_mp4 *m, uint64_t pos, uint32_t bytes)
{
    struct box b; uint64_t at=0; int r;
    if(pos>m->bytes || bytes>m->bytes-pos) return PLAYER_MEDIA_BAD_FORMAT;
    if(pos>=m->mdat_start && pos<=m->mdat_end && bytes<=m->mdat_end-pos) return 0;
    while(at<m->bytes) {
        r=get_box(m,at,m->bytes,&b); if(r) return r;
        if(b.type==FCC('m','d','a','t') && pos>=b.data && pos<=b.end && bytes<=b.end-pos) {m->mdat_start=b.data; m->mdat_end=b.end; return 0;}
        at=b.end;
    }
    return PLAYER_MEDIA_BAD_FORMAT;
}
static int setup_chunk(struct player_mp4 *m, struct track *t)
{
    const uint8_t *p; uint32_t next; int r;
    if(t->chunk>=t->tables[T_CHUNK].count) return PLAYER_MEDIA_BAD_FORMAT;
    if(t->map_index+1u<t->tables[T_MAP].count) {
        r=entry(m,&t->tables[T_MAP],t->map_index+1u,&p); if(r) return r;
        next=be32(p);
        if(t->chunk+1u>=next) t->map_index++;
    }
    r=entry(m,&t->tables[T_MAP],t->map_index,&p); if(r) return r;
    t->samples_per_chunk=be32(p+4);
    r=entry(m,&t->tables[T_CHUNK],t->chunk,&p); if(r) return r;
    t->chunk_offset=t->tables[T_CHUNK].stride==8u?be64(p):be32(p);
    return 0;
}
static int scan_video(struct player_mp4 *m, struct track *t, struct player_mp4_packet *packet, uint8_t *out)
{
    static const uint8_t aud[6]={0,0,0,1,9,0xf0};
    uint32_t pos=0,n,prefix=t->info.nal_length_bytes,normalized=6u,vcl=0,idr=0,auds=0,write=6u,i;
    uint8_t h[5]; int r;
    /* First pass validates and sizes the complete sample before touching dst. */
    while(pos<packet->source_bytes) {
        if(packet->source_bytes-pos<prefix+1u) return PLAYER_MEDIA_BAD_FORMAT;
        r=read_at(m,packet->source_offset+pos,h,prefix+1u); if(r) return r;
        n=0; for(i=0;i<prefix;i++) n=(n<<8)|h[i];
        pos+=prefix;
        if(!n || n>packet->source_bytes-pos || (h[prefix]&128u)) return PLAYER_MEDIA_BAD_FORMAT;
        i=h[prefix]&31u;
        if(i==9u) {if(++auds>1u) return PLAYER_MEDIA_BAD_FORMAT;}
        else {
            if(n>PLAYER_MEDIA_VIDEO_MAX-4u || normalized>PLAYER_MEDIA_VIDEO_MAX-4u-n) return PLAYER_MEDIA_UNSUPPORTED;
            normalized+=4u+n;
        }
        if(i>=1u && i<=5u) vcl++;
        if(i==5u) idr=1u;
        pos+=n;
    }
    if(!vcl) return PLAYER_MEDIA_BAD_FORMAT;
    if(idr) {
        if(t->headers_bytes>PLAYER_MEDIA_VIDEO_MAX-normalized) return PLAYER_MEDIA_UNSUPPORTED;
        normalized+=t->headers_bytes; packet->flags|=PLAYER_MP4_PACKET_IDR;
    }
    if(packet->normalized_bytes && packet->normalized_bytes!=normalized) return PLAYER_MEDIA_BAD_FORMAT;
    packet->normalized_bytes=normalized;
    if(!out) return 0;
    memcpy(out,aud,sizeof(aud));
    if(idr) {memcpy(out+write,t->headers,t->headers_bytes); write+=t->headers_bytes;}
    pos=0;
    while(pos<packet->source_bytes) {
        r=read_at(m,packet->source_offset+pos,h,prefix+1u); if(r) return r;
        n=0; for(i=0;i<prefix;i++) n=(n<<8)|h[i]; pos+=prefix;
        if((h[prefix]&31u)!=9u) {
            out[write++]=0; out[write++]=0; out[write++]=0; out[write++]=1;
            r=read_at(m,packet->source_offset+pos,out+write,n); if(r) return r;
            write+=n;
        }
        pos+=n;
    }
    return write==normalized?0:PLAYER_MEDIA_BAD_FORMAT;
}
static int peek_track(struct player_mp4 *m, struct track *t, uint32_t kind)
{
    struct player_mp4_packet *p=&t->peek; const uint8_t *row; uint32_t sync; int r;
    if(!t->present || t->index>=t->info.sample_count) return PLAYER_MEDIA_EOF;
    if(t->peek_valid) return 0;
    memset(p,0,sizeof(*p)); p->kind=kind==0u?PLAYER_PACKET_VIDEO:PLAYER_PACKET_AUDIO;
    if(!t->in_chunk) {r=setup_chunk(m,t); if(r) return r;}
    if(!t->time_left) {
        r=entry(m,&t->tables[T_TIME],t->time_index++,&row); if(r) return r;
        t->time_left=be32(row); t->sample_duration=be32(row+4);
    }
    if(t->info.has_ctts && !t->ctime_left) {
        r=entry(m,&t->tables[T_CTIME],t->ctime_index++,&row); if(r) return r;
        t->ctime_left=be32(row); t->composition_offset=t->ctts_signed?(int64_t)(int32_t)be32(row+4):(int64_t)be32(row+4);
    }
    p->source_bytes=t->fixed_size;
    if(!p->source_bytes) {r=get_u32(m,&t->tables[T_SIZE],t->index,&p->source_bytes); if(r) return r;}
    if(!p->source_bytes || t->chunk_offset>UINT64_MAX-t->chunk_used) return PLAYER_MEDIA_BAD_FORMAT;
    p->source_offset=t->chunk_offset+t->chunk_used;
    r=sample_in_mdat(m,p->source_offset,p->source_bytes); if(r) return r;
    p->sample_index=t->index; p->dts=t->decode_time;
    if(t->composition_offset>0 && p->dts>INT64_MAX-t->composition_offset) return PLAYER_MEDIA_UNSUPPORTED;
    p->pts=p->dts+t->composition_offset; p->timescale=t->info.timescale; p->duration=t->sample_duration;
    r=map_us(m,t,p->dts,&p->dts_us); if(r) return r;
    r=map_us(m,t,p->pts,&p->pts_us); if(r) return r;
    p->duration_us=((uint64_t)p->duration*1000000u)/p->timescale;
    if(!t->tables[T_SYNC].stride) p->flags|=PLAYER_MP4_PACKET_SYNC;
    else if(t->sync_index<t->tables[T_SYNC].count) {
        r=get_u32(m,&t->tables[T_SYNC],t->sync_index,&sync); if(r) return r;
        if(sync==t->index+1u) {p->flags|=PLAYER_MP4_PACKET_SYNC; t->sync_index++;}
    }
    if(kind==0u) {r=scan_video(m,t,p,NULL); if(r) return r;}
    else {if(p->source_bytes>PLAYER_MP4_AUDIO_MAX) return PLAYER_MEDIA_UNSUPPORTED; p->normalized_bytes=p->source_bytes;}
    t->peek_valid=1; return 0;
}
int player_mp4_next(struct player_mp4 *m, struct player_mp4_packet *packet)
{
    int a,b; struct track *t;
    if(!m || !packet) return PLAYER_MEDIA_BAD_FORMAT;
    a=peek_track(m,&m->tracks[0],0); if(a<0) return a;
    b=peek_track(m,&m->tracks[1],1); if(b<0) return b;
    if(a==PLAYER_MEDIA_EOF && b==PLAYER_MEDIA_EOF) return PLAYER_MEDIA_EOF;
    /* Stable tie: video first. File layout never controls queue interleave. */
    if(b==PLAYER_MEDIA_EOF || (a==0 && m->tracks[0].peek.dts_us<=m->tracks[1].peek.dts_us)) t=&m->tracks[0]; else t=&m->tracks[1];
    *packet=t->peek;
    t->index++; t->in_chunk++; t->chunk_used+=packet->source_bytes;
    if(t->in_chunk==t->samples_per_chunk) {t->chunk++; t->in_chunk=0; t->chunk_used=0;}
    t->decode_time+=t->sample_duration; t->time_left--; if(t->ctime_left) t->ctime_left--;
    t->peek_valid=0;
    return 0;
}
int player_mp4_read_packet(struct player_mp4 *m, const struct player_mp4_packet *packet, void *buffer, uint32_t capacity, uint32_t *written)
{
    struct player_mp4_packet checked; uint8_t *dst=(uint8_t *)buffer; int r;
    if(written) *written=0;
    if(!m || !packet || !buffer || !written || !packet->normalized_bytes) return PLAYER_MEDIA_BAD_FORMAT;
    if(capacity<PLAYER_MEDIA_INPUT_PADDING || packet->normalized_bytes>capacity-PLAYER_MEDIA_INPUT_PADDING) return PLAYER_MEDIA_BUFFER_SMALL;
    checked=*packet;
    if(packet->kind==PLAYER_PACKET_VIDEO && m->tracks[0].present) r=scan_video(m,&m->tracks[0],&checked,dst);
    else if(packet->kind==PLAYER_PACKET_AUDIO && m->tracks[1].present && packet->source_bytes==packet->normalized_bytes && packet->source_bytes<=PLAYER_MP4_AUDIO_MAX) r=read_at(m,packet->source_offset,dst,packet->source_bytes);
    else return PLAYER_MEDIA_BAD_FORMAT;
    if(r) return r;
    memset(dst+packet->normalized_bytes,0,PLAYER_MEDIA_INPUT_PADDING);
    *written=packet->normalized_bytes; return 0;
}

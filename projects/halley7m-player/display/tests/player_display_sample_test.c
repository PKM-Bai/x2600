#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include "player_rotator.h"

static unsigned depth, reads;
static bool initialized = true, running = true, rotated = true, rotator_enabled = true;
static bool rotator_stopping, pending_valid, overlay_queued, video_active;
static unsigned route_phase;
static int route_error;
static struct { struct { int comp_stopped; } CompData; } lcd = {{1}};
static player_rotator_t rotator;
static uint32_t *memory;
#define taskENTER_CRITICAL() (++depth)
#define taskEXIT_CRITICAL() (--depth)
static uint32_t read_pixel(uint32_t physical)
{
    assert(depth == 1);
    assert(physical >= 0x1000000u && physical < 0x1000000u + 1280u * 720u * 4u);
    ++reads;
    return memory[(physical - 0x1000000u) / 4u];
}
#define PLAYER_DISPLAY_SAMPLE_READ_WORD(physical) read_pixel(physical)
#include "player_display_sample_derived.inc"

int main(void)
{
    const player_display_sample_point_t points[] = {{0,0}, {128,0}, {0,1}, {0,8}, {1279,719}};
    player_display_sample_t sample;
    uint32_t offset;
    memory = malloc(1280u * 720u * 4u);
    assert(memory);
    for (uint32_t i = 0; i < 1280u * 720u; ++i) memory[i] = 0xff000000u | i;
    rotator.layout = (player_rotator_layout_t){1280,720,1280,1280u*720u*4u,0,0};
    rotator.latest_started = 1;
    rotator.slots[1] = (player_rotator_slot_t){0x1000000u,37,PLAYER_ROT_PRESENT|PLAYER_ROT_READING};
    assert(!player_display_sample_custom(points,5,0,&sample));
    assert(!depth && reads==5 && sample.sequence==37 && sample.physical==0x1000000u && sample.slot==1);
    assert(sample.argb[0]==memory[0] && sample.argb[1]==memory[92160] &&
           sample.argb[2]==memory[128] && sample.argb[3]==memory[1024] &&
           sample.argb[4]==memory[1280u*720u-1u]);
    assert(!display_sample_offset(1280,720,80,180,PLAYER_DISPLAY_SAMPLE_CUSTOM512,&offset) && offset==92480u);
    assert(!display_sample_offset(1280,720,1200,180,PLAYER_DISPLAY_SAMPLE_CUSTOM512,&offset) && offset==3410112u);
    assert(!player_display_sample_custom(points,5,1,&sample));
    assert(sample.argb[1]==memory[128] && sample.argb[2]==memory[1280]);
    player_display_sample_point_t batch[PLAYER_DISPLAY_SAMPLE_MAX];
    for (unsigned i=0; i<PLAYER_DISPLAY_SAMPLE_MAX; ++i)
        batch[i]=(player_display_sample_point_t){80u+64u*i,180};
    reads=0;
    assert(!player_display_sample_custom(batch,PLAYER_DISPLAY_SAMPLE_MAX,0,&sample));
    assert(!depth && reads==PLAYER_DISPLAY_SAMPLE_MAX && sample.count==PLAYER_DISPLAY_SAMPLE_MAX);
    batch[PLAYER_DISPLAY_SAMPLE_MAX-1u].x=1280;
    reads=0;
    assert(player_display_sample_custom(batch,PLAYER_DISPLAY_SAMPLE_MAX,0,&sample)==-EINVAL && !depth && !reads);
    assert(player_display_sample_custom(points,17,0,&sample)==-EINVAL && !depth);
    assert(player_display_sample_custom(points,0,0,&sample)==-EINVAL && !depth);
    assert(player_display_sample_custom(points,1,2,&sample)==-EINVAL && !depth);
    assert(display_sample_offset(1280,720,1280,0,0,&offset)==-EINVAL);
    /* The hardware-proven CUSTOM block map and the raw linear diagnostic
     * bridge must both stay bounded bijections, without alias or overflow. */
    for (unsigned model=0; model<2; ++model) {
        unsigned char *seen=calloc(1280u*720u,1);
        assert(seen);
        for (uint32_t y=0; y<720; ++y) for (uint32_t x=0; x<1280; ++x) {
            assert(!display_sample_offset(1280,720,x,y,model,&offset));
            assert(offset<1280u*720u*4u && !(offset&3u) && !seen[offset/4u]);
            seen[offset/4u]=1;
        }
        free(seen);
    }
    reads=0; video_active=true;
    assert(player_display_sample_custom(points,5,0,&sample)==-EAGAIN && !depth && !reads);
    video_active=false; pending_valid=true;
    assert(player_display_sample_custom(points,5,0,&sample)==-EAGAIN && !depth && !reads);
    pending_valid=false; route_phase=1;
    assert(player_display_sample_custom(points,5,0,&sample)==-EAGAIN && !depth && !reads);
    route_phase=0; overlay_queued=true;
    assert(player_display_sample_custom(points,5,0,&sample)==-EAGAIN && !depth && !reads);
    overlay_queued=false; rotator_stopping=true;
    assert(player_display_sample_custom(points,5,0,&sample)==-EAGAIN && !depth && !reads);
    rotator_stopping=false; rotator.latest_started=-1;
    assert(player_display_sample_custom(points,5,0,&sample)==-EAGAIN && !depth && !reads);
    rotator.latest_started=1; rotator.layout.bytes=0;
    assert(player_display_sample_custom(points,5,0,&sample)==-EOVERFLOW && !depth && !reads);
    rotator.layout.bytes=1280u*720u*4u; rotator.slots[1].flags=PLAYER_ROT_WRITING;
    assert(player_display_sample_custom(points,5,0,&sample)==-EAGAIN && !depth && !reads);
    rotator.slots[1].flags=0;
    assert(player_display_sample_custom(points,5,0,&sample)==-EAGAIN && !depth && !reads);
    rotator.slots[1].flags=PLAYER_ROT_LAST; lcd.CompData.comp_stopped=0;
    assert(player_display_sample_custom(points,5,0,&sample)==-EAGAIN && !depth && !reads);
    lcd.CompData.comp_stopped=1; rotated=false;
    assert(player_display_sample_custom(points,5,0,&sample)==-ENOTSUP && !depth && !reads);
    free(memory);
    puts("PASS sampler bounds, CUSTOM512 golden offsets/bijection, held-surface snapshot and no DMA read in busy/native/unowned states");
    return 0;
}

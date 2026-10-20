#include "player_rotator.h"
#include <assert.h>
#include <errno.h>
#include <stdio.h>

int main(void)
{
    player_rotator_layout_t l;
    assert(player_rotator_layout(1280,720,&l)==0);
    assert(l.bytes==3686400 && l.stride==1280 && l.frame_size==0x02d00500);
    assert(l.global==0x03000010);
    assert(player_rotator_layout(720,1280,&l)==-EINVAL);
    assert(player_rotator_layout(UINT32_MAX,720,&l)==-EINVAL);
    assert(player_rotator_layout(1280,721,&l)==-EINVAL);
    const uint32_t addresses[]={0x01000000,0x01400000,0x01800000};
    player_rotator_t r={0};
    assert(player_rotator_init(&r,1280,720,addresses)==0);
    player_rotator_token_t a={0},b={0},c={0},extra={0};
    assert(player_rotator_acquire(&r,&a)==0 && player_rotator_acquire(&r,&b)==0);
    assert(player_rotator_acquire(&r,&c)==0 && player_rotator_acquire(&r,&extra)==-EAGAIN);
    assert(!player_rotator_display_ready(&r,a));
    assert(player_rotator_written(&r,a)==0 && !player_rotator_display_ready(&r,a));
    assert(player_rotator_queue(&r,a)==0 && !player_rotator_display_ready(&r,a));
    assert(player_rotator_sof(&r,addresses[1])<0);
    assert(player_rotator_sof(&r,addresses[0]+64)==0);
    uint32_t held_flags=r.slots[a.slot].flags;
    assert(player_rotator_sof(&r,addresses[0]+128)==0); /* Held-frame SOF, no new command. */
    assert(r.reading==(int)a.slot && r.queued==-1 && r.slots[a.slot].flags==held_flags);
    assert(player_rotator_sof(&r,addresses[1])<0 && r.slots[a.slot].flags==held_flags);
    assert(player_rotator_sof(&r,addresses[0]+l.bytes)<0); /* Exclusive plane end. */
    assert(player_rotator_eof(&r)==0);
    assert(player_rotator_sof(&r,addresses[0]+256)==0); /* EOF+SOF, same PRESENT slot. */
    assert(player_rotator_display_ready(&r,a));
    assert(player_rotator_displayed(&r,a)==0);
    assert(player_rotator_eof(&r)==0);
    assert(player_rotator_repeat(&r,&extra)==0 && extra.slot==a.slot);
    assert(player_rotator_sof(&r,addresses[0])==0);
    assert(player_rotator_written(&r,b)==0 && player_rotator_queue(&r,b)==0);
    assert(player_rotator_sof(&r,addresses[0]+512)==0);
    assert(r.queued==(int)b.slot && r.reading==(int)a.slot);
    assert((r.slots[b.slot].flags&PLAYER_ROT_QUEUED)!=0); /* Old SOF must not consume new command. */
    assert(player_rotator_sof(&r,addresses[1])==-EINVAL); /* New address still waits for old EOF. */
    assert(player_rotator_eof(&r)==0);
    assert(player_rotator_sof(&r,addresses[0]+768)==0 && r.queued==(int)b.slot);
    assert(player_rotator_eof(&r)==0);
    assert(player_rotator_sof(&r,addresses[1])==0);
    assert(player_rotator_displayed(&r,b)==0);
    assert(player_rotator_acquire(&r,&extra)==0 && extra.slot==a.slot);
    assert(player_rotator_written(&r,a)<0); /* Old per-slot sequence cannot release a new write. */
    assert(player_rotator_cancel_write(&r,extra)==0);
    assert(player_rotator_reset(&r,false)==-EBUSY && r.slots[b.slot].flags);
    assert(player_rotator_reset(&r,true)==0);
    assert(player_rotator_written(&r,c)<0 && player_rotator_displayed(&r,b)<0);
    assert(player_rotator_sof(&r,addresses[1])==-EINVAL); /* No held address after idle/reset. */
    assert(player_rotator_acquire(&r,&a)==0 && player_rotator_acquire(&r,&b)==0);
    assert(player_rotator_written(&r,a)==0 && player_rotator_queue(&r,a)==0);
    assert(player_rotator_sof(&r,addresses[a.slot])==0);
    assert(player_rotator_written(&r,b)==0 && player_rotator_queue(&r,b)==0);
    assert(player_rotator_resolve_start(&r,b,addresses[b.slot],false)==0);
    assert(r.reading==(int)a.slot && r.queued==(int)b.slot); /* Missing EOF retains both holds. */
    assert(player_rotator_resolve_start(&r,b,addresses[a.slot]+64u,true)==0);
    assert(player_rotator_resolve_start(&r,b,addresses[c.slot]+64u,true)<0);
    player_rotator_token_t stale=b; ++stale.sequence;
    assert(player_rotator_resolve_start(&r,stale,addresses[b.slot],true)<0);
    assert(r.reading==(int)a.slot && r.queued==(int)b.slot);
    assert(player_rotator_resolve_start(&r,b,addresses[b.slot]+64u,true)==1);
    assert(r.reading==(int)b.slot && r.queued==-1 && !(r.slots[a.slot].flags&PLAYER_ROT_READING));
    assert(player_rotator_display_ready(&r,b));
    assert(player_rotator_reset(&r,true)==0);
    uint32_t bad[]={0x01000001,0x01400000,0x01800000};
    assert(player_rotator_init(&r,1280,720,bad)==-EINVAL);
    bad[0]=bad[1];
    assert(player_rotator_init(&r,1280,720,bad)==-EINVAL);
    bad[0]=0x03fff000;
    assert(player_rotator_init(&r,1280,720,bad)==-EINVAL);
    puts("PASS rotator: CUSTOM register layout, guards, EOW/SOF/EOD separation, repeat, three-slot holds, stale tokens, stop pin");
    return 0;
}

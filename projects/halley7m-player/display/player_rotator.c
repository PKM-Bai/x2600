#include "player_rotator.h"
#include <errno.h>
#include <string.h>
#ifndef ESTALE
#define ESTALE 116
#endif

int player_rotator_layout(uint32_t w, uint32_t h, player_rotator_layout_t *out)
{
    if (!out || !w || !h || w > 2047 || h > 2047 || (w & 127u) || (h & 7u)) return -EINVAL;
    *out = (player_rotator_layout_t){w, h, w, w*h*4u, w | (h << 16), (3u << 24) | (1u << 4)};
    return 0;
}

static player_rotator_slot_t *find(player_rotator_t *r, player_rotator_token_t t)
{
    if (t.epoch != r->epoch || t.slot >= PLAYER_ROTATOR_SLOTS ||
        t.sequence != r->slots[t.slot].sequence) return NULL;
    return &r->slots[t.slot];
}

static player_rotator_token_t token_for(player_rotator_t *r, unsigned slot)
{ return (player_rotator_token_t){r->epoch, r->slots[slot].sequence, slot}; }

int player_rotator_init(player_rotator_t *r, uint32_t w, uint32_t h, const uint32_t p[3])
{
    if (!r || !p) return -EINVAL;
    player_rotator_layout_t layout;
    int result = player_rotator_layout(w, h, &layout);
    if (result) return result;
    for (unsigned i=0; i<3; ++i) {
        if (!p[i] || (p[i]&511u) || p[i] > 0x04000000u-layout.bytes) return -EINVAL;
        for (unsigned j=0; j<i; ++j)
            if (p[i] < p[j]+layout.bytes && p[j] < p[i]+layout.bytes) return -EINVAL;
    }
    uint32_t epoch=r->epoch+1u;
    memset(r,0,sizeof(*r));
    r->epoch=epoch ? epoch : 1u;
    r->layout=layout;
    r->queued=r->reading=r->last=r->latest_started=-1;
    for (unsigned i=0; i<3; ++i) r->slots[i].physical=p[i];
    return 0;
}

int player_rotator_acquire(player_rotator_t *r, player_rotator_token_t *t)
{
    if (!r || !t) return -EINVAL;
    for (unsigned i=0; i<3; ++i) if (!r->slots[i].flags) {
        player_rotator_slot_t *s=&r->slots[i];
        if (++s->sequence == 0) ++s->sequence;
        s->flags=PLAYER_ROT_WRITING;
        *t=token_for(r,i);
        return 0;
    }
    return -EAGAIN;
}

int player_rotator_cancel_write(player_rotator_t *r, player_rotator_token_t t)
{
    player_rotator_slot_t *s=find(r,t);
    if (!s) return -ESTALE;
    if (s->flags!=PLAYER_ROT_WRITING) return -EBUSY;
    s->flags=0;
    return 0;
}

int player_rotator_written(player_rotator_t *r, player_rotator_token_t t)
{
    player_rotator_slot_t *s=find(r,t);
    if (!s) return -ESTALE;
    if (s->flags!=PLAYER_ROT_WRITING) return -EINVAL;
    s->flags=PLAYER_ROT_READY|PLAYER_ROT_PRESENT;
    return 0;
}

int player_rotator_queue(player_rotator_t *r, player_rotator_token_t t)
{
    player_rotator_slot_t *s=find(r,t);
    if (!s) return -ESTALE;
    if (r->queued>=0) return -EAGAIN;
    if (!(s->flags&(PLAYER_ROT_READY|PLAYER_ROT_LAST|PLAYER_ROT_PRESENT)) ||
        (s->flags&(PLAYER_ROT_WRITING|PLAYER_ROT_QUEUED|PLAYER_ROT_READING))) return -EINVAL;
    s->flags=(s->flags&~PLAYER_ROT_READY)|PLAYER_ROT_QUEUED;
    r->queued=(int)t.slot;
    return 0;
}

int player_rotator_sof(player_rotator_t *r, uint32_t site)
{
    if (!r) return -EINVAL;
    player_rotator_slot_t *next=r->queued>=0 ? &r->slots[r->queued] : NULL;
    if (!next || site<next->physical || site-next->physical>=r->layout.bytes) {
        /* The display port may scan the held address again without fetching a
         * new command. Accept only the same fully written, still pinned slot.
         * This also makes a repeated observation of its SOF harmless: it does
         * not end a read or release any buffer. A newer queued address remains
         * queued until its own SOF/site is observed. Never infer a new slot. */
        int slot=r->reading>=0 ? r->reading : r->latest_started;
        if (slot<0 || slot>=(int)PLAYER_ROTATOR_SLOTS) return -EINVAL;
        player_rotator_slot_t *held=&r->slots[slot];
        if (!(held->flags&(PLAYER_ROT_LAST|PLAYER_ROT_PRESENT)) ||
            (held->flags&~(PLAYER_ROT_READING|PLAYER_ROT_LAST|PLAYER_ROT_PRESENT))) return -EINVAL;
        if (site<held->physical || site-held->physical>=r->layout.bytes) return -ESTALE;
        held->flags|=PLAYER_ROT_READING;
        r->reading=r->latest_started=slot;
        return 0;
    }
    if (r->reading>=0) return -EINVAL;
    next->flags=(next->flags&~PLAYER_ROT_QUEUED)|PLAYER_ROT_READING;
    r->reading=r->latest_started=r->queued;
    r->queued=-1;
    return 0;
}

int player_rotator_eof(player_rotator_t *r)
{
    if (r->reading<0) return -EINVAL;
    r->slots[r->reading].flags &= ~PLAYER_ROT_READING;
    r->reading=-1;
    return 0;
}

int player_rotator_resolve_start(player_rotator_t *r, player_rotator_token_t t,
                                 uint32_t site, bool old_read_ended)
{
    player_rotator_slot_t *next=find(r,t);
    if (!next || r->queued!=(int)t.slot || !(next->flags&PLAYER_ROT_QUEUED)) return -ESTALE;
    if (site<next->physical || site-next->physical>=r->layout.bytes) {
        int old=r->reading>=0 ? r->reading : r->latest_started;
        if (old>=0 && site>=r->slots[old].physical &&
            site-r->slots[old].physical<r->layout.bytes) return 0;
        return -ESTALE;
    }
    if (r->reading>=0) {
        if (!old_read_ended) return 0;
        int result=player_rotator_eof(r);
        if (result) return result;
    }
    int result=player_rotator_sof(r,site);
    return result ? result : 1;
}

bool player_rotator_display_ready(const player_rotator_t *r, player_rotator_token_t t)
{
    return t.epoch==r->epoch && t.slot<3 && t.sequence==r->slots[t.slot].sequence &&
           (int)t.slot==r->latest_started && (r->slots[t.slot].flags&PLAYER_ROT_PRESENT)!=0;
}

int player_rotator_displayed(player_rotator_t *r, player_rotator_token_t t)
{
    if (!player_rotator_display_ready(r,t)) return -ESTALE;
    if (r->last>=0) r->slots[r->last].flags &= ~PLAYER_ROT_LAST;
    r->slots[t.slot].flags=(r->slots[t.slot].flags&~PLAYER_ROT_PRESENT)|PLAYER_ROT_LAST;
    r->last=(int)t.slot;
    return 0;
}

int player_rotator_repeat(player_rotator_t *r, player_rotator_token_t *t)
{
    if (!t || r->queued>=0 || r->reading>=0 || r->latest_started<0) return -EAGAIN;
    *t=token_for(r,(unsigned)r->latest_started);
    return player_rotator_queue(r,*t);
}

int player_rotator_reset(player_rotator_t *r, bool idle)
{
    if (!idle) return -EBUSY;
    for (unsigned i=0; i<3; ++i) r->slots[i].flags=0;
    if (++r->epoch==0) ++r->epoch;
    r->queued=r->reading=r->last=r->latest_started=-1;
    return 0;
}

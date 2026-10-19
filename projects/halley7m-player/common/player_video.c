#include "player_video.h"
#include <errno.h>
#include <limits.h>
#include <stddef.h>
#include <string.h>

int player_video_layout_nv12(uint32_t width, uint32_t height,
                              player_video_layout_t *layout)
{
    if (!layout) return -EINVAL;
    memset(layout, 0, sizeof(*layout));
    if (!width || !height || (width & 1u) || (height & 1u)) return -EINVAL;
    uint64_t stride = ((uint64_t)width + PLAYER_VIDEO_ALIGNMENT - 1u) &
                      ~((uint64_t)PLAYER_VIDEO_ALIGNMENT - 1u);
    uint64_t y_bytes = stride * height;
    /* Check the plane before adding sizes to avoid uint64_t addition overflow. */
    if (stride > UINT32_MAX || y_bytes > UINT32_MAX) return -EOVERFLOW;
    uint64_t total = y_bytes + y_bytes / 2u;
    if (total > UINT32_MAX) return -EOVERFLOW;
    layout->width = width;
    layout->height = height;
    layout->stride = (uint32_t)stride;
    layout->y_bytes = (uint32_t)y_bytes;
    layout->uv_offset = (uint32_t)y_bytes;
    layout->uv_bytes = (uint32_t)(y_bytes / 2u);
    layout->total_bytes = (uint32_t)total;
    return 0;
}

bool player_video_buffer_valid(const player_video_layout_t *layout,
                                uintptr_t base, uint32_t capacity)
{
    return layout && layout->total_bytes && base &&
           !(base & (PLAYER_VIDEO_ALIGNMENT - 1u)) &&
           capacity >= layout->total_bytes &&
           base <= UINTPTR_MAX - (layout->total_bytes - 1u);
}

static player_video_slot_t *find_slot(player_video_pool_t *pool,
                                       player_video_token_t token)
{
    if (token.slot >= PLAYER_VIDEO_SLOTS || token.session != pool->session)
        return NULL;
    player_video_slot_t *slot = pool->slots + token.slot;
    return slot->generation == token.generation ? slot : NULL;
}

void player_video_pool_init(player_video_pool_t *pool)
{
    memset(pool, 0, sizeof(*pool));
    pool->session = 1;
}

bool player_video_codec_acquire(player_video_pool_t *pool,
                                 player_video_token_t *token)
{
    for (uint32_t index = 0; index < PLAYER_VIDEO_SLOTS; ++index) {
        player_video_slot_t *slot = pool->slots + index;
        if (slot->holds) continue;
        ++slot->generation;
        slot->holds = PLAYER_VIDEO_CODEC;
        token->session = pool->session;
        token->generation = slot->generation;
        token->slot = index;
        return true;
    }
    return false;
}

bool player_video_codec_publish(player_video_pool_t *pool,
                                 player_video_token_t token)
{
    player_video_slot_t *slot = find_slot(pool, token);
    if (!slot || slot->holds != PLAYER_VIDEO_CODEC) return false;
    slot->holds |= PLAYER_VIDEO_READY;
    return true;
}

bool player_video_codec_release(player_video_pool_t *pool,
                                 player_video_token_t token)
{
    player_video_slot_t *slot = find_slot(pool, token);
    if (!slot || !(slot->holds & PLAYER_VIDEO_CODEC)) return false;
    slot->holds &= ~PLAYER_VIDEO_CODEC;
    return true;
}

bool player_video_discard_ready(player_video_pool_t *pool,
                                 player_video_token_t token)
{
    player_video_slot_t *slot = find_slot(pool, token);
    if (!slot || !(slot->holds & PLAYER_VIDEO_READY)) return false;
    slot->holds &= ~PLAYER_VIDEO_READY;
    return true;
}

bool player_video_display_submit(player_video_pool_t *pool,
                                  player_video_token_t token)
{
    player_video_slot_t *slot = find_slot(pool, token);
    if (!slot || !(slot->holds & PLAYER_VIDEO_READY)) return false;
    for (unsigned index = 0; index < PLAYER_VIDEO_SLOTS; ++index)
        if (pool->slots[index].holds & PLAYER_VIDEO_PENDING) return false;
    slot->holds = (slot->holds & ~PLAYER_VIDEO_READY) | PLAYER_VIDEO_PENDING;
    return true;
}

bool player_video_display_latch(player_video_pool_t *pool,
                                 player_video_token_t token)
{
    player_video_slot_t *slot = find_slot(pool, token);
    if (!slot || !(slot->holds & PLAYER_VIDEO_PENDING)) return false;
    for (unsigned index = 0; index < PLAYER_VIDEO_SLOTS; ++index)
        pool->slots[index].holds &= ~PLAYER_VIDEO_SCANOUT;
    slot->holds = (slot->holds & ~PLAYER_VIDEO_PENDING) | PLAYER_VIDEO_SCANOUT;
    return true;
}

bool player_video_display_release(player_video_pool_t *pool,
                                   player_video_token_t token)
{
    player_video_slot_t *slot = find_slot(pool, token);
    uint32_t display_holds = PLAYER_VIDEO_PENDING | PLAYER_VIDEO_SCANOUT;
    if (!slot || !(slot->holds & display_holds)) return false;
    slot->holds &= ~display_holds;
    return true;
}

void player_video_pool_stop(player_video_pool_t *pool)
{
    ++pool->session;
    for (unsigned index = 0; index < PLAYER_VIDEO_SLOTS; ++index)
        pool->slots[index].holds = 0;
}

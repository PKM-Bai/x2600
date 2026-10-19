#include "player_video.h"
#include <errno.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

static unsigned failures;

static void expect(bool condition, const char *name)
{
    if (!condition) {
        fprintf(stderr, "FAIL player_video: %s\n", name);
        ++failures;
    }
}

static void check_layout_and_guards(void)
{
    player_video_layout_t layout;
    static uint8_t storage[1474560u + 384u];
    uintptr_t base = ((uintptr_t)storage + 255u) & ~(uintptr_t)127u;
    uint8_t *pixels = (uint8_t *)base;
    expect(player_video_layout_nv12(720, 1280, &layout) == 0, "720x1280 layout");
    expect(layout.width == 720 && layout.height == 1280 && layout.stride == 768,
           "fixed portrait dimensions and padded stride");
    expect(layout.y_bytes == 983040 && layout.uv_offset == 983040 &&
           layout.uv_bytes == 491520 && layout.total_bytes == 1474560,
           "independent plane byte counts");
    expect(player_video_buffer_valid(&layout, base, 1474560), "exact capacity");
    expect(!player_video_buffer_valid(&layout, base, 1474559), "short capacity");
    expect(!player_video_buffer_valid(&layout, base + 1u, 1474560), "unaligned base");
    expect(!player_video_buffer_valid(&layout, 0, 1474560), "null base");
    expect(!player_video_buffer_valid(&layout, UINTPTR_MAX & ~(uintptr_t)127u,
                                     1474560), "address span overflow");
    memset(storage, 0xa5, sizeof(storage));
    for (uint32_t row = 0; row < layout.height; ++row)
        memset(pixels + (size_t)row * layout.stride, 0x11, layout.width);
    for (uint32_t row = 0; row < layout.height / 2u; ++row)
        memset(pixels + layout.uv_offset + (size_t)row * layout.stride,
               0x88, layout.width);
    size_t start = (size_t)(pixels - storage);
    bool guards_ok = true;
    for (size_t index = 0; index < sizeof(storage); ++index) {
        uint8_t expected = 0xa5;
        if (index >= start && index - start < 1474560u) {
            size_t offset = index - start;
            /* Independent literal geometry checks both planes and row padding. */
            if (offset % 768u < 720u)
                expected = offset < 983040u ? 0x11 : 0x88;
        }
        if (storage[index] != expected) { guards_ok = false; break; }
    }
    expect(guards_ok, "Y/UV plane bounds, stride padding and allocation guards");
    expect(player_video_layout_nv12(128, 2, &layout) == 0 &&
           layout.stride == 128 && layout.total_bytes == 384, "aligned width");
    expect(player_video_layout_nv12(130, 2, &layout) == 0 &&
           layout.stride == 256 && layout.uv_offset == 512 &&
           layout.total_bytes == 768, "width rounds upward");
    expect(player_video_layout_nv12(0, 2, &layout) == -EINVAL, "zero width");
    expect(player_video_layout_nv12(2, 0, &layout) == -EINVAL, "zero height");
    expect(player_video_layout_nv12(719, 1280, &layout) == -EINVAL, "odd width");
    expect(player_video_layout_nv12(720, 1279, &layout) == -EINVAL, "odd height");
    expect(player_video_layout_nv12(UINT32_MAX - 1u, 2, &layout) == -EOVERFLOW,
           "stride overflow");
    expect(player_video_layout_nv12(128, UINT32_MAX - 1u, &layout) == -EOVERFLOW,
           "Y plane multiplication overflow");
    expect(player_video_layout_nv12(65536, 49152, &layout) == -EOVERFLOW,
           "combined Y and UV size overflow");
    expect(!layout.total_bytes && !player_video_buffer_valid(&layout, base,
           UINT32_MAX), "failed layout is empty and cannot bind");
    expect(player_video_layout_nv12(720, 1280, NULL) == -EINVAL, "null layout");
}

static void check_exhaustion_and_shared_holds(void)
{
    player_video_pool_t pool;
    player_video_token_t frames[4], extra;
    player_video_pool_init(&pool);
    for (unsigned index = 0; index < 4; ++index) {
        expect(player_video_codec_acquire(&pool, frames + index), "acquire four slots");
        expect(frames[index].slot == index, "unique slot");
    }
    expect(!player_video_codec_acquire(&pool, &extra), "four-slot exhaustion");
    expect(!player_video_display_submit(&pool, frames[0]), "unpublished frame rejected");
    expect(player_video_codec_publish(&pool, frames[0]), "codec publishes ready frame");
    expect(!player_video_codec_publish(&pool, frames[0]), "duplicate publish rejected");
    expect(player_video_display_submit(&pool, frames[0]), "ready becomes pending");
    expect(pool.slots[0].holds == (PLAYER_VIDEO_CODEC | PLAYER_VIDEO_PENDING),
           "decoder and pending display both retain frame");
    expect(!player_video_display_latch(&pool, frames[1]), "wrong latch rejected");
    expect(player_video_display_latch(&pool, frames[0]), "hardware latch");
    expect(pool.slots[0].holds == (PLAYER_VIDEO_CODEC | PLAYER_VIDEO_SCANOUT),
           "codec and scanout share frame");
    expect(player_video_codec_release(&pool, frames[0]), "decoder drops DPB hold");
    expect(pool.slots[0].holds == PLAYER_VIDEO_SCANOUT, "scanout remains pinned");
    expect(!player_video_codec_acquire(&pool, &extra), "scanout not reusable");
    expect(player_video_codec_publish(&pool, frames[1]), "next ready frame");
    expect(player_video_display_submit(&pool, frames[1]), "next pending frame");
    expect(player_video_codec_publish(&pool, frames[2]), "third ready frame");
    expect(!player_video_display_submit(&pool, frames[2]), "only one pending frame");
    expect(player_video_display_latch(&pool, frames[1]), "latch retires old scanout");
    expect(!pool.slots[0].holds, "old scanout now reusable");
    expect(player_video_codec_acquire(&pool, &extra) && extra.slot == 0,
           "reuse retired slot");
    expect(extra.generation != frames[0].generation, "reuse increments slot generation");
    expect(!player_video_codec_release(&pool, frames[0]), "stale codec release rejected");
    expect(!player_video_display_release(&pool, frames[0]), "stale DPU release rejected");
    expect(pool.slots[0].holds == PLAYER_VIDEO_CODEC, "stale callbacks preserve new hold");
    expect(player_video_display_release(&pool, frames[1]), "display retires before codec");
    expect(pool.slots[1].holds == PLAYER_VIDEO_CODEC, "decoder still pins retired display");
    expect(player_video_codec_release(&pool, frames[1]), "decoder finally releases slot");
    expect(player_video_discard_ready(&pool, frames[2]), "drop late ready frame");
    expect(pool.slots[2].holds == PLAYER_VIDEO_CODEC, "drop ready preserves DPB hold");
    expect(player_video_codec_release(&pool, frames[2]), "release dropped frame");
    expect(!player_video_codec_release(&pool, frames[2]), "double release rejected");
    expect(!player_video_discard_ready(&pool, frames[2]), "double discard rejected");
}

static void check_stop_and_pending_cancel(void)
{
    player_video_pool_t pool;
    player_video_token_t old, current, invalid;
    player_video_pool_init(&pool);
    expect(player_video_codec_acquire(&pool, &old), "acquire before stop");
    expect(player_video_codec_publish(&pool, old), "publish before stop");
    expect(player_video_display_submit(&pool, old), "pending before stop");
    player_video_pool_stop(&pool); /* Test driver has already quiesced hardware. */
    expect(player_video_codec_acquire(&pool, &current), "acquire after stop");
    expect(current.session != old.session && current.slot == old.slot,
           "stop advances session before reusing slot");
    expect(!player_video_codec_publish(&pool, old) &&
           !player_video_codec_release(&pool, old) &&
           !player_video_discard_ready(&pool, old) &&
           !player_video_display_submit(&pool, old) &&
           !player_video_display_latch(&pool, old) &&
           !player_video_display_release(&pool, old), "old-session operations rejected");
    expect(pool.slots[current.slot].holds == PLAYER_VIDEO_CODEC,
           "old session cannot recycle new frame");
    invalid = current;
    invalid.slot = PLAYER_VIDEO_SLOTS;
    expect(!player_video_codec_release(&pool, invalid), "invalid slot rejected");
    expect(player_video_codec_publish(&pool, current), "publish after stop");
    expect(player_video_display_submit(&pool, current), "submit after stop");
    expect(player_video_display_release(&pool, current), "cancel hardware-retired pending");
    expect(pool.slots[current.slot].holds == PLAYER_VIDEO_CODEC,
           "cancel pending preserves codec hold");
    expect(!player_video_display_latch(&pool, current), "canceled pending cannot latch");
    expect(player_video_codec_release(&pool, current), "final codec release");
    expect(!player_video_display_release(&pool, current), "double display release rejected");
}

int main(void)
{
    check_layout_and_guards();
    check_exhaustion_and_shared_holds();
    check_stop_and_pending_cancel();
    if (failures) return 1;
    puts("player_video PASS: NV12 stride/planes/guards/alignment/overflow, "
         "four slots, codec+DPU holds, stale tokens, stopped sessions");
    return 0;
}

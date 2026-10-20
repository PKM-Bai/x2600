#include "player_codec_workspace.h"
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int require(int condition, const char *message)
{
    if (!condition) { fprintf(stderr, "FAIL %s\n", message); return 0; }
    return 1;
}

static int check_plane(const uint8_t *bytes, uint32_t size, uint8_t value)
{
    for (uint32_t i = 0; i < size; ++i)
        if (bytes[i] != value) return 0;
    return 1;
}

static void fill_regions(const player_codec_workspace_layout_t *layout,
                         const player_codec_workspace_regions_t *regions, uint8_t value)
{
    memset(regions->ctrl, value, layout->ctrl_bytes);
    memset(regions->mv, (uint8_t)(value + 1u), layout->mv_bytes);
    memset(regions->y, (uint8_t)(value + 2u), layout->y_bytes);
    memset(regions->uv, (uint8_t)(value + 3u), layout->uv_bytes);
}

static int check_regions(const player_codec_workspace_layout_t *layout,
                         const player_codec_workspace_regions_t *regions, uint8_t value)
{
    return check_plane(regions->ctrl, layout->ctrl_bytes, value) &&
           check_plane(regions->mv, layout->mv_bytes, (uint8_t)(value + 1u)) &&
           check_plane(regions->y, layout->y_bytes, (uint8_t)(value + 2u)) &&
           check_plane(regions->uv, layout->uv_bytes, (uint8_t)(value + 3u));
}

static int exercise(uint32_t width, uint32_t height)
{
    static const uint32_t retire_order[] = {4, 1, 8, 0, 6, 2, 7, 3, 5};
    player_codec_workspace_layout_t layout;
    player_codec_workspace_t workspace;
    player_codec_workspace_regions_t regions[PLAYER_VIDEO_SLOTS];
    player_codec_workspace_regions_t again;
    uint8_t *allocation, *arena;
    int success = 0;
    if (!require(!player_codec_workspace_layout(width, height, PLAYER_VIDEO_SLOTS, &layout), "coded layout") ||
        !require(layout.ctrl_bytes == width / 16u * (height / 16u) * 8u + 256u &&
                 layout.mv_bytes == width / 16u * (height / 16u) * 128u + 256u &&
                 layout.y_bytes == width * height && layout.uv_bytes == width * height / 2u,
                 "exact vendor side-data and tiled-plane sizes") ||
        !require(!(layout.ctrl_offset & 255u) && !(layout.mv_offset & 255u) &&
                 !(layout.y_offset & 255u) && !(layout.uv_offset & 255u) &&
                 !(layout.slot_bytes & 255u), "every DMA plane and slot aligned") ||
        !require(layout.ctrl_offset + layout.ctrl_bytes <= layout.mv_offset &&
                 layout.mv_offset + layout.mv_bytes <= layout.y_offset &&
                 layout.y_offset + layout.y_bytes <= layout.uv_offset &&
                 layout.uv_offset + layout.uv_bytes <= layout.slot_bytes &&
                 layout.total_bytes == PLAYER_VIDEO_SLOTS * layout.slot_bytes, "non-overlapping layout")) return 0;
    allocation = malloc((size_t)layout.total_bytes + 1024u);
    if (!require(allocation != NULL, "single host arena allocation")) return 0;
    memset(allocation, 0xa5, (size_t)layout.total_bytes + 1024u);
    arena = (uint8_t *)(((uintptr_t)allocation + 511u) & ~(uintptr_t)255u);
    if (!require(!player_codec_workspace_init(&workspace, &layout, arena, layout.total_bytes), "bind arena")) goto done;
    for (uint32_t i = 0; i < PLAYER_VIDEO_SLOTS; ++i) {
        if (!require(!player_codec_workspace_acquire(&workspace, i, &regions[i]), "acquire distinct frame slot")) goto done;
        fill_regions(&layout, &regions[i], (uint8_t)(i * 8u));
    }
    if (!require(workspace.active_count == PLAYER_VIDEO_SLOTS && workspace.peak_active == PLAYER_VIDEO_SLOTS,
                 "bounded full occupancy")) goto done;
    for (uint32_t i = 0; i < PLAYER_VIDEO_SLOTS; ++i) {
        if (!require(player_codec_workspace_acquire(&workspace, i, &again) == -EBUSY,
                     "exhausted slots cannot be rebound while referenced") ||
            !require(check_regions(&layout, &regions[i], (uint8_t)(i * 8u)), "other slots never overlap")) goto done;
    }
    /* Reuse references in display/DPB retirement order, rather than FIFO order.
     * The returned addresses must remain stable while all other slots are live. */
    for (uint32_t cycle = 0; cycle < 200u; ++cycle) {
        uint32_t slot = retire_order[cycle % PLAYER_VIDEO_SLOTS];
        if (!require(!player_codec_workspace_release(&workspace, slot), "retire out of order") ||
            !require(player_codec_workspace_release(&workspace, slot) == -EINVAL, "double return rejected") ||
            !require(!player_codec_workspace_acquire(&workspace, slot, &again), "reacquire without allocating") ||
            !require(again.ctrl == regions[slot].ctrl && again.mv == regions[slot].mv &&
                     again.y == regions[slot].y && again.uv == regions[slot].uv, "stable per-frame physical layout") ||
            !require(check_regions(&layout, &again, (uint8_t)(slot * 8u)), "data survives unrelated retirement")) goto done;
    }
    for (uint32_t i = 0; i < PLAYER_VIDEO_SLOTS; ++i)
        if (!require(!player_codec_workspace_release(&workspace, retire_order[i]), "all references retire")) goto done;
    if (!require(!workspace.active_mask && !workspace.active_count && workspace.peak_active == PLAYER_VIDEO_SLOTS,
                 "complete unwind retains peak statistic") ||
        !require(check_plane(arena - 256u, 256u, 0xa5) &&
                 check_plane(arena + layout.total_bytes, 256u, 0xa5), "arena guards intact")) goto done;
    success = 1;
done:
    free(allocation);
    return success;
}

int main(void)
{
    player_codec_workspace_layout_t layout, corrupt;
    player_codec_workspace_t workspace;
    uint8_t *allocation, *arena;
    player_codec_workspace_regions_t regions;
    if (!require(player_codec_workspace_layout(0, 720, 9, &layout) == -EINVAL &&
                 player_codec_workspace_layout(1280, 721, 9, &layout) == -EINVAL &&
                 player_codec_workspace_layout(1280, 720, 0, &layout) == -EINVAL &&
                 player_codec_workspace_layout(1280, 720, PLAYER_VIDEO_SLOTS + 1u, &layout) == -EINVAL,
                 "invalid geometry and slot counts") ||
        !require(player_codec_workspace_layout(1920, 1088, 9, &layout) == -ENOTSUP &&
                 player_codec_workspace_layout(0xfffffff0u, 0xfffffff0u, 9, &layout) == -ENOTSUP,
                 "coded-pixel budget checked before narrowing") ||
        !require(!player_codec_workspace_layout(1280, 720, PLAYER_VIDEO_SLOTS, &layout), "720p layout")) return 1;
    allocation = malloc((size_t)layout.total_bytes + 512u);
    if (!require(allocation != NULL, "failure test allocation")) return 1;
    arena = (uint8_t *)(((uintptr_t)allocation + 255u) & ~(uintptr_t)255u);
    corrupt = layout; ++corrupt.uv_offset;
    if (!require(player_codec_workspace_init(&workspace, &layout, arena, layout.total_bytes - 1u) == -ENOMEM,
                 "short arena fails before binding") ||
        !require(player_codec_workspace_init(&workspace, &layout, arena + 1, layout.total_bytes) == -EINVAL &&
                 player_codec_workspace_init(&workspace, &corrupt, arena, layout.total_bytes) == -EINVAL,
                 "reject misalignment and forged layout") ||
        !require(player_codec_workspace_init(&workspace, &layout,
                     (void *)(UINTPTR_MAX & ~(uintptr_t)255u), layout.total_bytes) == -EOVERFLOW,
                 "arena address overflow") ||
        !require(!player_codec_workspace_init(&workspace, &layout, arena, layout.total_bytes) &&
                 player_codec_workspace_acquire(&workspace, PLAYER_VIDEO_SLOTS, &regions) == -EINVAL &&
                 player_codec_workspace_release(&workspace, 0) == -EINVAL && !workspace.active_mask,
                 "invalid ownership does not change occupancy")) { free(allocation); return 1; }
    free(allocation);
    if (!exercise(1280, 720) || !exercise(720, 1280) || !exercise(640, 368) ||
        !exercise(864, 480) || !exercise(960, 544) || !exercise(720, 576)) return 1;
    printf("PASS fixed FELIX arena: %u slots, 720p bytes=%u; aligned planes, exhaustion, out-of-order reuse, guards and unwind\n",
           PLAYER_VIDEO_SLOTS, layout.total_bytes);
    return 0;
}

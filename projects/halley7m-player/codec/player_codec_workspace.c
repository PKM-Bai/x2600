#include "player_codec_workspace.h"
#include <errno.h>
#include <limits.h>
#include <string.h>

static uint64_t align_workspace(uint64_t value)
{
    return (value + PLAYER_CODEC_WORKSPACE_ALIGNMENT - 1u) &
           ~((uint64_t)PLAYER_CODEC_WORKSPACE_ALIGNMENT - 1u);
}

int player_codec_workspace_layout(uint32_t width, uint32_t height, uint32_t slots,
                                  player_codec_workspace_layout_t *layout)
{
    uint64_t pixels, macroblocks, ctrl, mv, mv_offset, y_offset, uv_offset, slot_bytes, total;
    player_codec_workspace_layout_t value;
    if (!layout) return -EINVAL;
    memset(layout, 0, sizeof(*layout));
    if (!width || !height || (width & 15u) || (height & 15u) ||
        !slots || slots > PLAYER_VIDEO_SLOTS || slots > 32u) return -EINVAL;
    pixels = (uint64_t)width * height;
    if (pixels > 1280u * 720u) return -ENOTSUP;
    macroblocks = pixels / 256u;
    ctrl = macroblocks * 8u + 256u;
    mv = macroblocks * 128u + 256u;
    mv_offset = align_workspace(ctrl);
    y_offset = align_workspace(mv_offset + mv);
    uv_offset = align_workspace(y_offset + pixels);
    slot_bytes = align_workspace(uv_offset + pixels / 2u);
    total = slot_bytes * slots;
    if (slot_bytes > UINT32_MAX || total > UINT32_MAX) return -EOVERFLOW;
    memset(&value, 0, sizeof(value));
    value.width = width; value.height = height; value.slots = slots;
    value.ctrl_bytes = (uint32_t)ctrl; value.mv_bytes = (uint32_t)mv;
    value.y_bytes = (uint32_t)pixels; value.uv_bytes = (uint32_t)(pixels / 2u);
    value.ctrl_offset = 0;
    value.mv_offset = (uint32_t)mv_offset;
    value.y_offset = (uint32_t)y_offset;
    value.uv_offset = (uint32_t)uv_offset;
    value.slot_bytes = (uint32_t)slot_bytes;
    value.total_bytes = (uint32_t)total;
    *layout = value;
    return 0;
}

int player_codec_workspace_init(player_codec_workspace_t *workspace,
                                const player_codec_workspace_layout_t *layout,
                                void *arena, size_t capacity)
{
    player_codec_workspace_layout_t expected;
    int result;
    if (!workspace || !layout || !arena ||
        ((uintptr_t)arena & (PLAYER_CODEC_WORKSPACE_ALIGNMENT - 1u))) return -EINVAL;
    result = player_codec_workspace_layout(layout->width, layout->height, layout->slots, &expected);
    if (result) return result;
    if (memcmp(layout, &expected, sizeof(expected))) return -EINVAL;
    if (capacity < layout->total_bytes) return -ENOMEM;
    if ((uintptr_t)arena > UINTPTR_MAX - (layout->total_bytes - 1u)) return -EOVERFLOW;
    memset(workspace, 0, sizeof(*workspace));
    workspace->layout = *layout;
    workspace->arena = arena;
    return 0;
}

int player_codec_workspace_acquire(player_codec_workspace_t *workspace, uint32_t slot,
                                   player_codec_workspace_regions_t *regions)
{
    uint8_t *base;
    uint32_t bit;
    if (!workspace || !workspace->arena || !regions || slot >= workspace->layout.slots)
        return -EINVAL;
    bit = 1u << slot;
    if (workspace->active_mask & bit) return -EBUSY;
    base = workspace->arena + (size_t)slot * workspace->layout.slot_bytes;
    regions->ctrl = base + workspace->layout.ctrl_offset;
    regions->mv = base + workspace->layout.mv_offset;
    regions->y = base + workspace->layout.y_offset;
    regions->uv = base + workspace->layout.uv_offset;
    workspace->active_mask |= bit;
    ++workspace->active_count;
    if (workspace->active_count > workspace->peak_active)
        workspace->peak_active = workspace->active_count;
    return 0;
}

int player_codec_workspace_release(player_codec_workspace_t *workspace, uint32_t slot)
{
    uint32_t bit;
    if (!workspace || !workspace->arena || slot >= workspace->layout.slots) return -EINVAL;
    bit = 1u << slot;
    if (!(workspace->active_mask & bit)) return -EINVAL;
    workspace->active_mask &= ~bit;
    --workspace->active_count;
    return 0;
}

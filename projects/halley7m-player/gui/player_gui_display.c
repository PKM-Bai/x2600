#include "player_gui.h"
#include "player_gui_surface.h"
#include "player_display.h"
#include <cache.h>

static int submit(void *context, const uint32_t *argb, uint32_t bytes, uint32_t serial,
                  unsigned x1, unsigned y1, unsigned x2, unsigned y2)
{
    (void)context;
    /* Only the GUI owns this back surface. Publish the changed native rows;
     * it cannot contain FELIX DMA output, unlike the NV12 video buffer. */
    for (unsigned y = y1; y <= y2; ++y)
        CleanDCache_by_Addr((unsigned long *)(argb + y * PLAYER_GUI_NATIVE_WIDTH + x1),
                           (x2 - x1 + 1u) * sizeof(uint32_t));
    return player_display_submit_overlay(argb, bytes, serial);
}

static int poll(void *context, uint32_t *serial)
{
    (void)context;
    return player_display_poll_overlay_completion(serial);
}

const player_gui_output_t *player_gui_display_output(void)
{
    static const player_gui_output_t backend = {NULL, submit, poll};
    return &backend;
}

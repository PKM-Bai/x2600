#include "player_gui_surface.h"
#include <string.h>

void player_gui_surface_init(player_gui_surface_t *s, uint32_t *a, uint32_t *b)
{
    memset(s, 0, sizeof(*s));
    s->pixels[0] = a;
    s->pixels[1] = b;
    s->front = -1;
    memset(a, 0, PLAYER_GUI_SURFACE_BYTES);
    memset(b, 0, PLAYER_GUI_SURFACE_BYTES);
}

bool player_gui_surface_reconfigure(player_gui_surface_t *s, bool logical)
{
    if (s->submitted) return false;
    uint32_t *a=s->pixels[0], *b=s->pixels[1], serial=s->serial;
    player_gui_surface_init(s,a,b);
    s->serial=serial;
    s->logical=logical;
    return true;
}

bool player_gui_surface_write(player_gui_surface_t *s, int x1, int y1,
                              int x2, int y2, const uint32_t *argb)
{
    if (s->sealed || !argb || x1 < 0 || y1 < 0 || x2 < x1 || y2 < y1 ||
        x2 >= (int)PLAYER_GUI_WIDTH || y2 >= (int)PLAYER_GUI_HEIGHT) return false;
    unsigned source_width = (unsigned)(x2 - x1 + 1);
    for (int y = y1; y <= y2; ++y)
        for (int x = x1; x <= x2; ++x)
            s->pixels[s->back][s->logical ? (unsigned)y*PLAYER_GUI_WIDTH+(unsigned)x :
                (unsigned)x*PLAYER_GUI_NATIVE_WIDTH+719u-(unsigned)y] =
                argb[(unsigned)(y - y1) * source_width + (unsigned)(x - x1)];
    unsigned nx1 = 719u - (unsigned)y2, nx2 = 719u - (unsigned)y1;
    unsigned ny1 = (unsigned)x1, ny2 = (unsigned)x2;
    if (s->logical) { nx1=(unsigned)x1; nx2=(unsigned)x2; ny1=(unsigned)y1; ny2=(unsigned)y2; }
    if (!s->dirty) { s->x1 = nx1; s->x2 = nx2; s->y1 = ny1; s->y2 = ny2; }
    else {
        if (nx1 < s->x1) s->x1 = nx1;
        if (nx2 > s->x2) s->x2 = nx2;
        if (ny1 < s->y1) s->y1 = ny1;
        if (ny2 > s->y2) s->y2 = ny2;
    }
    if (!s->publish_dirty) {
        s->publish_x1 = nx1; s->publish_x2 = nx2;
        s->publish_y1 = ny1; s->publish_y2 = ny2;
    } else {
        if (nx1 < s->publish_x1) s->publish_x1 = nx1;
        if (nx2 > s->publish_x2) s->publish_x2 = nx2;
        if (ny1 < s->publish_y1) s->publish_y1 = ny1;
        if (ny2 > s->publish_y2) s->publish_y2 = ny2;
    }
    s->publish_dirty = true;
    s->dirty = true;
    return true;
}

bool player_gui_surface_seal(player_gui_surface_t *s)
{
    if (!s->dirty || s->sealed) return false;
    if (++s->serial == 0) ++s->serial;
    s->sealed = true;
    return true;
}

bool player_gui_surface_submitted(player_gui_surface_t *s)
{
    if (!s->sealed || s->submitted) return false;
    s->submitted = true;
    return true;
}

bool player_gui_surface_complete(player_gui_surface_t *s, uint32_t serial)
{
    if (!s->submitted || serial != s->serial) return false;
    unsigned old = s->back ^ 1u;
    for (unsigned y = s->y1; y <= s->y2; ++y) {
        unsigned offset = y * (s->logical ? PLAYER_GUI_WIDTH : PLAYER_GUI_NATIVE_WIDTH) + s->x1;
        memcpy(s->pixels[old] + offset, s->pixels[s->back] + offset,
               (s->x2 - s->x1 + 1u) * sizeof(uint32_t));
    }
    s->front = (int)s->back;
    s->back = old;
    /* This copy dirtied the retired back buffer's CPU cache. Its next submit
     * must publish these rows as well as that frame's newly rendered rows. */
    s->publish_x1 = s->x1; s->publish_x2 = s->x2;
    s->publish_y1 = s->y1; s->publish_y2 = s->y2;
    s->publish_dirty = true;
    s->dirty = s->sealed = s->submitted = false;
    return true;
}

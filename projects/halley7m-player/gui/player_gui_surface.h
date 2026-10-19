#ifndef PLAYER_GUI_SURFACE_H
#define PLAYER_GUI_SURFACE_H
#include <stdbool.h>
#include <stdint.h>
#define PLAYER_GUI_WIDTH 1280u
#define PLAYER_GUI_HEIGHT 720u
#define PLAYER_GUI_NATIVE_WIDTH 720u
#define PLAYER_GUI_NATIVE_HEIGHT 1280u
#define PLAYER_GUI_SURFACE_BYTES (720u * 1280u * 4u)

typedef struct {
    uint32_t *pixels[2];
    unsigned back;
    int front;
    uint32_t serial;
    unsigned x1, y1, x2, y2;
    unsigned publish_x1, publish_y1, publish_x2, publish_y2;
    bool dirty, sealed, submitted, publish_dirty;
} player_gui_surface_t;

void player_gui_surface_init(player_gui_surface_t *surface, uint32_t *a, uint32_t *b);
/* Source is a tightly packed logical LVGL rectangle, straight ARGB8888.
 * Clockwise mapping matches the PC video transpose: (x,y)->(719-y,x).
 * No current/pending scanout surface may be written. */
bool player_gui_surface_write(player_gui_surface_t *surface, int x1, int y1,
                              int x2, int y2, const uint32_t *argb);
bool player_gui_surface_seal(player_gui_surface_t *surface);
/* Call only after backend accepts this serial; -EAGAIN leaves it sealed. */
bool player_gui_surface_submitted(player_gui_surface_t *surface);
/* Matching IRQ-confirmed serial releases the old front. Copy the changed
 * native rectangle to it before it becomes the next writable back buffer. */
bool player_gui_surface_complete(player_gui_surface_t *surface, uint32_t serial);
#endif

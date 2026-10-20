#ifndef PLAYER_GUI_H
#define PLAYER_GUI_H
#include "player_input.h"
#include <stddef.h>
#include <stdint.h>

enum player_gui_page { PLAYER_GUI_LIBRARY, PLAYER_GUI_PLAYBACK, PLAYER_GUI_DIAGNOSTICS };
typedef struct {
    void *context;
    /* Submit straight ARGB in the acknowledged native/logical layout.
     * Return 0/-EAGAIN/error.
     * Pixels remain pinned until poll returns the matching serial. */
    int (*submit)(void *context, const uint32_t *argb, uint32_t bytes, uint32_t serial,
                  unsigned x1, unsigned y1, unsigned x2, unsigned y2);
    int (*poll)(void *context, uint32_t *serial); /* 1 completed, 0 none, <0 error */
    /* Optional route handshake: phase1 pause after draining, phase2 rebuild
     * after old DMA idle. Only the GUI task executes LVGL/surface changes. */
    int (*mode)(void *context, uint32_t *generation, bool *logical);
    int (*ack)(void *context, uint32_t generation, unsigned phase);
    void (*attach)(void *context, bool attached);
} player_gui_output_t;
typedef struct {
    enum player_gui_page page;
    unsigned menu_open, menu_selected, controls_visible;
    uint32_t generation, rendered, presented, dropped_repeats, dropped_actions;
    int output_error;
    uint32_t output_generation;
    bool logical_output;
} player_gui_stats_t;

/* Every call, all LVGL use and font scratch use belong to one GUI task.
 * init allocates two native surfaces + a 32-line LVGL draw buffer. */
int player_gui_init(const player_gui_output_t *output);
/* The 5ms input task may call this while GUI renders. It only runs the action
 * recognizer under a short lock; no LVGL use occurs outside the GUI task. */
void player_gui_sample_keys(uint32_t now_ms, uint16_t stable_keys);
int player_gui_step(uint32_t now_ms);
void player_gui_get_stats(player_gui_stats_t *stats);
/* Display must be stopped/quiescent before destroying buffers; caller states
 * that contract explicitly. Returns -EBUSY if quiescent is false. */
int player_gui_deinit(int display_quiescent);
/* Existing X2600 display driver adapter; host tests supply a memory backend. */
const player_gui_output_t *player_gui_display_output(void);
#endif

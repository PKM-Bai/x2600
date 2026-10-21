/* GT911 pointer input for the player. GPL-2.0-or-later. */
#ifndef PLAYER_TOUCH_H
#define PLAYER_TOUCH_H
#include <stdint.h>

typedef struct {
    uint16_t x, y;             /* Logical landscape coordinates: 1280 x 720. */
    uint16_t raw_x, raw_y;     /* Coordinates read from the touch controller. */
    uint8_t pressed, id;
} player_touch_sample_t;

typedef struct {
    uint32_t ready, address;
    char product[5];
    uint16_t firmware, width, height;
    uint32_t rotated, max_contacts;
    uint32_t polls, reports, presses, releases, errors, invalid_reports;
    uint32_t multiple_contacts;
    int last_error;
    player_touch_sample_t last_raw;
} player_touch_stats_t;

/* Init and poll belong to the GUI task. Missing touch is nonfatal to keys,
 * playback and display. Init only reads the controller's configuration. */
int player_touch_init(void);
/* Always fills sample, including a safe release on errors. Returns 1 for a new
 * report, 0 when no report is ready / the 10 ms period has not expired, or a
 * negative errno. Caller can forward sample on any nonzero result. */
int player_touch_poll(uint32_t now_ms, player_touch_sample_t *sample);
/* A short, consistent snapshot; also callable from the console task. */
void player_touch_get_stats(player_touch_stats_t *stats);

/* Pure mapping used independently of either display route. Native portrait
 * 720 x 1280 rotates to x=raw_y,y=719-raw_x. An already-landscape 1280 x 720
 * configuration maps directly. Other dimensions require explicit adaptation. */
int player_touch_map(uint16_t width, uint16_t height, uint16_t raw_x,
                     uint16_t raw_y, uint16_t *x, uint16_t *y);
#endif

/* Platform-independent GT911 protocol. GPL-2.0-or-later. */
#ifndef PLAYER_TOUCH_CORE_H
#define PLAYER_TOUCH_CORE_H
#include "player_touch.h"
#include <stddef.h>

#define PLAYER_GT911_CONFIG 0x8048u
#define PLAYER_GT911_ID 0x8140u
#define PLAYER_GT911_STATUS 0x814eu
#define PLAYER_GT911_CONTACTS 0x814fu
#define PLAYER_GT911_MAX_CONTACTS 10u

typedef struct {
    void *context;
    /* addr is 7-bit; reg is sent high byte first. Hardware adapter must bound
     * every transaction and use repeated START between register and data. */
    int (*read)(void *context, uint8_t addr, uint16_t reg, uint8_t *data, size_t size);
    int (*write)(void *context, uint8_t addr, uint16_t reg, const uint8_t *data, size_t size);
} player_touch_bus_t;

typedef struct {
    player_touch_bus_t bus;
    player_touch_stats_t stats;
    player_touch_sample_t sample;
    uint32_t next_poll;
    uint8_t poll_started;
} player_gt911_t;

int player_gt911_init(player_gt911_t *touch, const player_touch_bus_t *bus);
int player_gt911_poll(player_gt911_t *touch, uint32_t now_ms, player_touch_sample_t *sample);
#endif

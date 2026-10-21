/* GT911 wire format follows Linux v6.12 drivers/input/touchscreen/goodix.c:
 * 16-bit BE register addresses, LE config/coordinates, READY bit 7 at 814e,
 * 8-byte contact records and read-before-clear acknowledgement.
 * Protocol reference: https://github.com/torvalds/linux/blob/v6.12/drivers/input/touchscreen/goodix.c
 * Independently implemented for this player. GPL-2.0-or-later. */
#include "player_touch_core.h"
#include <errno.h>
#include <string.h>

static uint16_t le16(const uint8_t *p)
{
    return (uint16_t)(p[0] | (uint16_t)p[1] << 8);
}

int player_touch_map(uint16_t width, uint16_t height, uint16_t raw_x,
                     uint16_t raw_y, uint16_t *x, uint16_t *y)
{
    if (!x || !y) return -EINVAL;
    if (raw_x >= width || raw_y >= height) return -ERANGE;
    if (width == 720 && height == 1280) {
        *x = raw_y;
        *y = (uint16_t)(719u - raw_x);
    } else if (width == 1280 && height == 720) {
        *x = raw_x;
        *y = raw_y;
    } else return -ENOTSUP;
    return 0;
}

static void release(player_gt911_t *touch)
{
    if (touch->sample.pressed) ++touch->stats.releases;
    touch->sample.pressed = 0;
    touch->stats.last_raw = touch->sample;
}

static int failed(player_gt911_t *touch, int error, int invalid)
{
    ++touch->stats.errors;
    if (invalid) ++touch->stats.invalid_reports;
    touch->stats.last_error = error;
    release(touch);
    return error;
}

int player_gt911_init(player_gt911_t *touch, const player_touch_bus_t *bus)
{
    static const uint8_t addresses[] = {0x14, 0x5d};
    uint8_t id[6], config[5];
    unsigned i;
    int result = -ENODEV;
    if (!touch || !bus || !bus->read || !bus->write) return -EINVAL;
    memset(touch, 0, sizeof(*touch));
    touch->bus = *bus;
    for (i = 0; i < sizeof(addresses); ++i) {
        result = bus->read(bus->context, addresses[i], PLAYER_GT911_ID, id, sizeof(id));
        if (result) continue;
        if (id[0] != '9' || id[1] != '1' || id[2] != '1' ||
            (id[3] != 0 && id[3] != ' ')) {
            result = -ENODEV;
            continue;
        }
        touch->stats.address = addresses[i];
        memcpy(touch->stats.product, id, 4);
        touch->stats.firmware = le16(id + 4);
        result = bus->read(bus->context, addresses[i], PLAYER_GT911_CONFIG,
                           config, sizeof(config));
        if (result) break;
        touch->stats.width = le16(config);
        touch->stats.height = le16(config + 2);
        touch->stats.max_contacts = config[4] & 0x0fu;
        if (!touch->stats.max_contacts ||
            touch->stats.max_contacts > PLAYER_GT911_MAX_CONTACTS) {
            result = -ENOTSUP;
            break;
        }
        uint16_t x, y;
        result = player_touch_map(touch->stats.width, touch->stats.height, 0, 0, &x, &y);
        if (result) break;
        touch->stats.rotated = touch->stats.width == 720;
        touch->stats.ready = 1;
        return 0;
    }
    touch->stats.last_error = result;
    ++touch->stats.errors;
    return result;
}

static int read_report(player_gt911_t *touch)
{
    uint8_t status, contacts[PLAYER_GT911_MAX_CONTACTS * 8], clear = 0;
    unsigned count, i, selected = 0;
    int result;
    uint8_t addr = (uint8_t)touch->stats.address;
    ++touch->stats.polls;
    result = touch->bus.read(touch->bus.context, addr, PLAYER_GT911_STATUS, &status, 1);
    if (result) return failed(touch, result, 0);
    if (!(status & 0x80)) return 0; /* Do not clear a not-yet-ready report. */
    count = status & 0x0fu;
    if (count > touch->stats.max_contacts) {
        /* An invalid count cannot index the fixed report buffer. Discard this
         * ready report so a bad header cannot keep the interrupt asserted. */
        result = touch->bus.write(touch->bus.context, addr, PLAYER_GT911_STATUS, &clear, 1);
        return failed(touch, result ? result : -EPROTO, 1);
    }
    if (count) {
        result = touch->bus.read(touch->bus.context, addr, PLAYER_GT911_CONTACTS,
                                 contacts, count * 8u);
        if (result) return failed(touch, result, 0); /* Keep unread report ready. */
    }
    /* Acknowledge only after every reported contact has been read. */
    result = touch->bus.write(touch->bus.context, addr, PLAYER_GT911_STATUS, &clear, 1);
    if (result) return failed(touch, result, 0);
    ++touch->stats.reports;
    touch->stats.last_error = 0;
    if (!count) {
        release(touch);
        return 1;
    }
    if (count > 1) ++touch->stats.multiple_contacts;
    /* Keep the same finger while more fingers enter. Do not jump the pointer
     * onto a second finger when the original one lifts. */
    if (touch->sample.pressed) {
        for (i = 0; i < count; ++i)
            if ((contacts[i * 8u] & 0x0fu) == touch->sample.id) break;
        if (i == count) {
            release(touch);
            return 1;
        }
        selected = i;
    }
    uint16_t ids = 0;
    for (i = 0; i < count; ++i) {
        unsigned id = contacts[i * 8u] & 0x0fu;
        uint16_t x, y;
        if (ids & (uint16_t)(1u << id)) return failed(touch, -EPROTO, 1);
        ids |= (uint16_t)(1u << id);
        result = player_touch_map(touch->stats.width, touch->stats.height,
                                  le16(contacts + i * 8u + 1),
                                  le16(contacts + i * 8u + 3), &x, &y);
        if (result) return failed(touch, result, 1);
    }
    const uint8_t *point = contacts + selected * 8u;
    touch->sample.raw_x = le16(point + 1);
    touch->sample.raw_y = le16(point + 3);
    (void)player_touch_map(touch->stats.width, touch->stats.height,
                          touch->sample.raw_x, touch->sample.raw_y,
                          &touch->sample.x, &touch->sample.y);
    touch->sample.id = point[0] & 0x0fu;
    if (!touch->sample.pressed) ++touch->stats.presses;
    touch->sample.pressed = 1;
    touch->stats.last_raw = touch->sample;
    return 1;
}

int player_gt911_poll(player_gt911_t *touch, uint32_t now_ms, player_touch_sample_t *sample)
{
    int result;
    if (!touch || !sample) return -EINVAL;
    *sample = touch->sample;
    if (!touch->stats.ready) return -ENODEV;
    if (touch->poll_started && (int32_t)(now_ms - touch->next_poll) < 0) return 0;
    touch->poll_started = 1;
    result = read_report(touch);
    touch->next_poll = now_ms + (result < 0 ? 100u : 10u);
    *sample = touch->sample;
    return result;
}

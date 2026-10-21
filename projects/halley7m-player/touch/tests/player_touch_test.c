#include "player_touch_core.h"
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>

typedef struct {
    uint8_t address, status, config[5], product[6], contacts[80];
    uint16_t fail_read, fail_write;
    unsigned reads, writes, contact_reads;
    unsigned sequence, data_sequence, clear_sequence;
} fake_t;

static int read_bus(void *context, uint8_t address, uint16_t reg, uint8_t *data, size_t size)
{
    fake_t *fake = context;
    ++fake->reads;
    ++fake->sequence;
    if (address != fake->address) return -ENXIO;
    if (reg == fake->fail_read) return -EIO;
    if (reg == PLAYER_GT911_ID) {
        assert(size == 6);
        memcpy(data, fake->product, size);
    } else if (reg == PLAYER_GT911_CONFIG) {
        assert(size == 5);
        memcpy(data, fake->config, size);
    } else if (reg == PLAYER_GT911_STATUS) {
        assert(size == 1);
        *data = fake->status;
    } else {
        assert(reg == PLAYER_GT911_CONTACTS);
        assert(size <= sizeof(fake->contacts) && !(size % 8));
        ++fake->contact_reads;
        fake->data_sequence = fake->sequence;
        memcpy(data, fake->contacts, size);
    }
    return 0;
}

static int write_bus(void *context, uint8_t address, uint16_t reg,
                     const uint8_t *data, size_t size)
{
    fake_t *fake = context;
    ++fake->sequence;
    assert(address == fake->address);
    /* Every write in both init and poll must be only the report ACK. */
    assert(reg == PLAYER_GT911_STATUS && size == 1 && *data == 0);
    if (reg == fake->fail_write) return -EIO;
    fake->clear_sequence = fake->sequence;
    ++fake->writes;
    fake->status = 0;
    return 0;
}

static void put16(uint8_t *data, unsigned value)
{
    data[0] = (uint8_t)value;
    data[1] = (uint8_t)(value >> 8);
}

static void prepare(fake_t *fake, unsigned address, unsigned width, unsigned height)
{
    memset(fake, 0, sizeof(*fake));
    fake->address = (uint8_t)address;
    memcpy(fake->product, "911\0\x60\x10", 6);
    put16(fake->config, width);
    put16(fake->config + 2, height);
    fake->config[4] = 5;
}

static void contact(fake_t *fake, unsigned index, unsigned id, unsigned x, unsigned y)
{
    uint8_t *data = fake->contacts + index * 8;
    data[0] = (uint8_t)id;
    put16(data + 1, x);
    put16(data + 3, y);
}

static void mapping(void)
{
    uint16_t x, y;
    assert(player_touch_map(720, 1280, 0, 0, &x, &y) == 0 && x == 0 && y == 719);
    assert(player_touch_map(720, 1280, 719, 0, &x, &y) == 0 && x == 0 && y == 0);
    assert(player_touch_map(720, 1280, 0, 1279, &x, &y) == 0 && x == 1279 && y == 719);
    assert(player_touch_map(720, 1280, 719, 1279, &x, &y) == 0 && x == 1279 && y == 0);
    assert(player_touch_map(1280, 720, 1279, 719, &x, &y) == 0 && x == 1279 && y == 719);
    assert(player_touch_map(1280, 720, 1280, 0, &x, &y) == -ERANGE);
    assert(player_touch_map(720, 1280, 0, 1280, &x, &y) == -ERANGE);
    assert(player_touch_map(800, 480, 10, 10, &x, &y) == -ENOTSUP);
    assert(player_touch_map(720, 1280, 0, 0, NULL, &y) == -EINVAL);
}

int main(void)
{
    player_gt911_t touch;
    player_touch_sample_t sample;
    fake_t fake;
    player_touch_bus_t bus = {&fake, read_bus, write_bus};
    mapping();
    prepare(&fake, 0x14, 720, 1280);
    assert(player_gt911_init(&touch, &bus) == 0);
    assert(fake.writes == 0 && touch.stats.ready && touch.stats.address == 0x14);
    assert(touch.stats.firmware == 0x1060 && touch.stats.rotated && touch.stats.width == 720);
    assert(!strcmp(touch.stats.product, "911"));

    /* Not ready is not an implicit release and must not be acknowledged. */
    fake.status = 1;
    assert(player_gt911_poll(&touch, 0, &sample) == 0 && !sample.pressed);
    assert(!fake.writes && !fake.contact_reads);
    contact(&fake, 0, 3, 719, 1279);
    fake.status = 0x81;
    assert(player_gt911_poll(&touch, 9, &sample) == 0 && fake.status == 0x81);
    assert(player_gt911_poll(&touch, 10, &sample) == 1);
    assert(sample.pressed && sample.id == 3 && sample.x == 1279 && sample.y == 0);
    assert(fake.data_sequence < fake.clear_sequence && fake.status == 0);
    assert(touch.stats.presses == 1 && touch.stats.reports == 1);
    assert(player_gt911_poll(&touch, 20, &sample) == 0 && sample.pressed);
    assert(touch.stats.releases == 0);
    fake.status = 0x80;
    assert(player_gt911_poll(&touch, 30, &sample) == 1 && !sample.pressed);
    assert(touch.stats.releases == 1);

    /* First finger stays selected even if its record moves behind another. */
    contact(&fake, 0, 3, 100, 200);
    fake.status = 0x81;
    assert(player_gt911_poll(&touch, 40, &sample) == 1 && sample.id == 3);
    contact(&fake, 0, 2, 500, 600);
    contact(&fake, 1, 3, 110, 210);
    fake.status = 0x82;
    assert(player_gt911_poll(&touch, 50, &sample) == 1 && sample.id == 3 && sample.raw_x == 110);
    assert(touch.stats.multiple_contacts == 1);
    contact(&fake, 0, 2, 500, 600);
    fake.status = 0x81;
    assert(player_gt911_poll(&touch, 60, &sample) == 1 && !sample.pressed);

    /* Read failures never acknowledge unread coordinates; they release a
     * previously held pointer and back off instead of blocking the GUI loop. */
    fake.status = 0x81;
    assert(player_gt911_poll(&touch, 70, &sample) == 1 && sample.pressed);
    unsigned writes = fake.writes;
    fake.fail_read = PLAYER_GT911_CONTACTS;
    fake.status = 0x81;
    assert(player_gt911_poll(&touch, 80, &sample) == -EIO && !sample.pressed);
    assert(fake.writes == writes && fake.status == 0x81);
    unsigned reads = fake.reads;
    assert(player_gt911_poll(&touch, 179, &sample) == 0 && fake.reads == reads);
    fake.fail_read = 0;
    assert(player_gt911_poll(&touch, 180, &sample) == 1 && sample.pressed);

    /* Reject impossible counts before indexing, but consume a malformed ready
     * header so it cannot prevent subsequent valid releases. */
    fake.status = 0x86;
    unsigned contact_reads = fake.contact_reads;
    assert(player_gt911_poll(&touch, 190, &sample) == -EPROTO && !sample.pressed);
    assert(fake.contact_reads == contact_reads && fake.status == 0);
    assert(touch.stats.invalid_reports == 1);
    contact(&fake, 0, 1, 720, 100);
    fake.status = 0x81;
    assert(player_gt911_poll(&touch, 290, &sample) == -ERANGE && !sample.pressed);
    assert(fake.status == 0 && touch.stats.invalid_reports == 2);
    contact(&fake, 0, 1, 10, 10);
    contact(&fake, 1, 1, 20, 20);
    fake.status = 0x82;
    assert(player_gt911_poll(&touch, 390, &sample) == -EPROTO);
    assert(touch.stats.invalid_reports == 3);

    /* Failed ACK must not publish a new down event. Status stays ready. */
    fake.status = 0x81;
    fake.fail_write = PLAYER_GT911_STATUS;
    assert(player_gt911_poll(&touch, 490, &sample) == -EIO && !sample.pressed);
    assert(fake.status == 0x81);
    fake.fail_write = 0;
    assert(player_gt911_poll(&touch, 590, &sample) == 1 && sample.pressed);
    fake.fail_read = PLAYER_GT911_STATUS;
    assert(player_gt911_poll(&touch, 600, &sample) == -EIO && !sample.pressed);

    /* Alternative address, landscape config and timer wrap. */
    prepare(&fake, 0x5d, 1280, 720);
    assert(player_gt911_init(&touch, &bus) == 0 && touch.stats.address == 0x5d);
    assert(fake.reads == 3 && !fake.writes && !touch.stats.rotated);
    fake.status = 0x81;
    contact(&fake, 0, 0, 1279, 719);
    assert(player_gt911_poll(&touch, UINT32_MAX - 5u, &sample) == 1);
    assert(sample.x == 1279 && sample.y == 719);
    fake.status = 0x80;
    assert(player_gt911_poll(&touch, 3, &sample) == 0 && sample.pressed);
    assert(player_gt911_poll(&touch, 4, &sample) == 1 && !sample.pressed);

    prepare(&fake, 0x14, 800, 480);
    assert(player_gt911_init(&touch, &bus) == -ENOTSUP && !touch.stats.ready && !fake.writes);
    prepare(&fake, 0x14, 720, 1280);
    fake.config[4] = 11;
    assert(player_gt911_init(&touch, &bus) == -ENOTSUP && !fake.writes);
    prepare(&fake, 0x14, 720, 1280);
    fake.config[4] = 10; /* The original HALLEY7-M config advertises ten. */
    assert(player_gt911_init(&touch, &bus) == 0 && touch.stats.max_contacts == 10);
    for (unsigned i = 0; i < 10; ++i) contact(&fake, i, i, 100 + i, 200 + i);
    fake.status = 0x8a;
    assert(player_gt911_poll(&touch, 0, &sample) == 1 && sample.pressed);
    assert(fake.status == 0 && touch.stats.multiple_contacts == 1);
    prepare(&fake, 0x14, 720, 1280);
    fake.product[0] = 'X';
    assert(player_gt911_init(&touch, &bus) < 0 && !touch.stats.ready && !fake.writes);
    assert(player_gt911_poll(&touch, 0, &sample) == -ENODEV && !sample.pressed);
    puts("PASS GT911 protocol: identify, read-before-clear, release, bounds, multitouch, IO errors, mapping, clock wrap");
    return 0;
}

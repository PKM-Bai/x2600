#ifndef AI_BOARD_GMAC_H
#define AI_BOARD_GMAC_H

#include <stdint.h>

typedef struct {
    uint32_t mac_version;
    uint32_t phy_mode;
    uint32_t mac_clock;
    uint32_t ahb0_hz;
    uint32_t ahb2_hz;
    uint32_t found_mask;
    uint16_t id1[32];
    uint16_t id2[32];
    uint16_t status[32];
} ai_gmac_probe_t;

/* Explicit diagnostic only: resets the external PHY, temporarily sets PC15-24
 * and the 50MHz PHY clock, scans MDIO, then restores pins/clock. Does not start
 * DMA or a netif. Do not invoke while a live Ethernet driver owns the hardware.
 * Returns 0, -EINVAL, -EBUSY, -ERANGE, -ETIMEDOUT, or -ENODEV. */
int ai_board_gmac_probe(ai_gmac_probe_t *report);

#endif

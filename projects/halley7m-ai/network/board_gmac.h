#ifndef AI_BOARD_GMAC_H
#define AI_BOARD_GMAC_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

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

typedef struct {
    bool available;
    uint32_t control;
    /* Cumulative owner-sampled counters. Retained across driver link resets. */
    uint32_t tx_frames_gb, tx_frames_good, tx_octets_gb, tx_1024_max, tx_underflow;
    uint32_t rx_frames_gb, rx_octets_gb, rx_octets_good, rx_1024_max;
    uint32_t rx_crc, rx_align, rx_runt, rx_jabber, rx_oversize_good, rx_overflow, rx_watchdog;
} ai_gmac_mmc_t;

typedef struct {
    bool initialized;
    bool link_up;
    bool full_duplex;
    uint8_t phy_address;
    unsigned speed_mbps;
    uint32_t phy_id;
    uint16_t bmsr;
    uint16_t bmcr;
    uint16_t advertisement;
    uint16_t partner;
    uint32_t mac_version;
    uint32_t hardware_features;
    uint32_t phy_mode;
    uint32_t dma_status;
    uint32_t last_rx_descriptor;
    uint32_t last_tx_descriptor;
    uint32_t rx_packets;
    uint32_t tx_packets;
    uint32_t rx_errors;
    uint32_t tx_errors;
    uint32_t rx_dropped;
    uint32_t tx_dropped;
    int last_error;
    ai_gmac_mmc_t mmc;
} ai_gmac_status_t;

/* Polling prototype; a SINGLE network task owns every call. No IRQs or locks.
 * init starts autonegotiation but does not wait for a cable/link. It preserves
 * the board's observed MACPHYC mode=2; frame operation needs board validation. */
int ai_board_gmac_init(const uint8_t mac[6]);
/* Call about every 250ms. 0 means the snapshot is valid, including link down. */
int ai_board_gmac_poll_link(void);
/* 0 accepts/copies one Ethernet frame (no FCS); success is not TX completion.
 * -EAGAIN leaves the caller's frame unconsumed. No automatic retransmission. */
int ai_board_gmac_send(const void *frame, size_t length);
/* >0 copied Ethernet frame length without FCS, 0 no frame, <0 error/drop.
 * A too-small destination drops that frame and returns -EMSGSIZE. */
int ai_board_gmac_read(void *frame, size_t capacity);
/* Also collects TX completion and MMC counters; owner only, no waiting. */
void ai_board_gmac_status(ai_gmac_status_t *report);
/* Bounded DMA reset before releasing pins/clock. A reset timeout retains
 * hardware ownership and static DMA storage; retry stop before init/probe. */
int ai_board_gmac_stop(void);

#endif

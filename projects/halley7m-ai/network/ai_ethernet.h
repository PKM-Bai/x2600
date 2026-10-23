#ifndef AI_ETHERNET_H
#define AI_ETHERNET_H
#include <stdbool.h>
#include <stdint.h>

typedef struct {
    bool started, link_up, address_ready, dns_ready, time_ready;
    int error, dns_result;
    unsigned speed_mbps;
    uint32_t generation, received, submitted, input_drops, output_drops;
    uint32_t arp_rx, arp_target_us, arp_rx_reply, arp_tx_reply;
    uint8_t mac[6];
    uint8_t gateway_mac[6];
    char address[16], netmask[16], gateway[16], dns_server[16], cloud_address[16];
    char last_arp_target[16];
} ai_ethernet_snapshot;

/* Start once after the scheduler. The network task alone owns the GMAC. */
int ai_ethernet_start(void);
void ai_ethernet_snapshot_get(ai_ethernet_snapshot *out);
void ai_ethernet_print_status(void);
#endif

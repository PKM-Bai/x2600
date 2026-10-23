#ifndef AI_PING_H
#define AI_PING_H

/* Explicit diagnostic from a normal task, never the tcpip task. Sends four
 * 1472-byte ICMP payloads only to the current DHCP gateway. Each response has
 * a one-second deadline. Returns 0 only when all four replies are verified,
 * otherwise negative errno. Prints metadata and RTT, never packet contents. */
int ai_ping_gateway(void);
/* Same bounded diagnostic with a fixed 32-byte payload for size comparison. */
int ai_ping_gateway_small(void);

#endif

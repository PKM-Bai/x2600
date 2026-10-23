#ifndef AI_PING_PACKET_H
#define AI_PING_PACKET_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define AI_PING_PAYLOAD_BYTES 1472u
#define AI_PING_SMALL_PAYLOAD_BYTES 32u
#define AI_PING_ICMP_BYTES (8u + AI_PING_PAYLOAD_BYTES)

static uint16_t ping_be16(const uint8_t *p)
{ return (uint16_t)((uint16_t)p[0] << 8 | p[1]); }

static void ping_write_be16(uint8_t *p, uint16_t value)
{ p[0] = (uint8_t)(value >> 8); p[1] = (uint8_t)value; }

static uint16_t ping_checksum(const uint8_t *p, size_t bytes)
{
    uint32_t sum = 0;
    while (bytes > 1) { sum += ping_be16(p); p += 2; bytes -= 2; }
    if (bytes) sum += (uint32_t)p[0] << 8;
    while (sum >> 16) sum = (sum & UINT32_C(0xffff)) + (sum >> 16);
    return (uint16_t)~sum;
}

static uint8_t ping_pattern(size_t offset, uint16_t id, uint16_t sequence)
{ return (uint8_t)(offset * 37u + (offset >> 8) + id + sequence * 13u); }

static void ping_make_request(uint8_t packet[AI_PING_ICMP_BYTES], size_t payload_bytes,
                              uint16_t id, uint16_t sequence)
{
    packet[0] = 8; packet[1] = 0; packet[2] = 0; packet[3] = 0;
    ping_write_be16(packet + 4, id); ping_write_be16(packet + 6, sequence);
    for (size_t i = 0; i < payload_bytes; ++i)
        packet[8 + i] = ping_pattern(i, id, sequence);
    ping_write_be16(packet + 2, ping_checksum(packet, 8u + payload_bytes));
}

/* lwIP RAW sockets include the IPv4 header. raw_recv happens before ICMP's
 * checksum check, so verify the ICMP checksum here as well as every data byte. */
static bool ping_reply_matches(const uint8_t *packet, size_t bytes, size_t payload_bytes,
                               uint16_t id, uint16_t sequence)
{
    if (payload_bytes != AI_PING_SMALL_PAYLOAD_BYTES && payload_bytes != AI_PING_PAYLOAD_BYTES)
        return false;
    if (bytes < 20 || packet[0] >> 4 != 4 || packet[9] != 1) return false;
    size_t ip_bytes = (size_t)(packet[0] & 15u) * 4u;
    size_t icmp_bytes = 8u + payload_bytes;
    if (ip_bytes < 20 || ip_bytes > 60 || bytes != ip_bytes + icmp_bytes ||
        ping_be16(packet + 2) != bytes || (ping_be16(packet + 6) & 0x3fffu)) return false;
    const uint8_t *icmp = packet + ip_bytes;
    if (icmp[0] != 0 || icmp[1] != 0 || ping_be16(icmp + 4) != id ||
        ping_be16(icmp + 6) != sequence || ping_checksum(icmp, icmp_bytes)) return false;
    for (size_t i = 0; i < payload_bytes; ++i)
        if (icmp[8 + i] != ping_pattern(i, id, sequence)) return false;
    return true;
}

#endif

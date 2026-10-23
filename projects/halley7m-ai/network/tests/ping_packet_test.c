#include "ping_packet.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static void test_size(size_t payload_bytes)
{
    uint8_t request[AI_PING_ICMP_BYTES], packet[1540] = {0};
    size_t icmp_bytes = payload_bytes + 8u, packet_bytes = payload_bytes + 28u;
    const uint16_t id = 0x8137, sequence = 4;
    ping_make_request(request, payload_bytes, id, sequence);
    assert(request[0] == 8 && !ping_checksum(request, icmp_bytes));
    packet[0] = 0x45; packet[9] = 1;
    ping_write_be16(packet + 2, (uint16_t)packet_bytes);
    memcpy(packet + 20, request, icmp_bytes);
    packet[20] = 0; packet[22] = packet[23] = 0;
    ping_write_be16(packet + 22, ping_checksum(packet + 20, icmp_bytes));
    assert(ping_reply_matches(packet, packet_bytes, payload_bytes, id, sequence));
    for (size_t bytes = 0; bytes < packet_bytes; ++bytes)
        assert(!ping_reply_matches(packet, bytes, payload_bytes, id, sequence));
    assert(!ping_reply_matches(packet, packet_bytes + 1, payload_bytes, id, sequence));
    assert(!ping_reply_matches(packet, packet_bytes, payload_bytes, (uint16_t)(id + 1), sequence));
    assert(!ping_reply_matches(packet, packet_bytes, payload_bytes, id, (uint16_t)(sequence + 1)));
    assert(!ping_reply_matches(packet, packet_bytes, payload_bytes == 32 ? 1472 : 32, id, sequence));
    packet[packet_bytes - 1] ^= 1;
    assert(!ping_reply_matches(packet, packet_bytes, payload_bytes, id, sequence));
    /* Even checksum-correct data from another message must be rejected. */
    packet[22] = packet[23] = 0;
    ping_write_be16(packet + 22, ping_checksum(packet + 20, icmp_bytes));
    assert(!ping_reply_matches(packet, packet_bytes, payload_bytes, id, sequence));
    packet[packet_bytes - 1] ^= 1; packet[22] = packet[23] = 0;
    ping_write_be16(packet + 22, ping_checksum(packet + 20, icmp_bytes));
    packet[0] = 0x44; assert(!ping_reply_matches(packet, packet_bytes, payload_bytes, id, sequence));
    packet[0] = 0x45; packet[6] = 0x20;
    assert(!ping_reply_matches(packet, packet_bytes, payload_bytes, id, sequence));
    packet[6] = 0;
    /* A valid IPv4 options header must not shift parsing into the payload. */
    memmove(packet + 24, packet + 20, icmp_bytes);
    memset(packet + 20, 0, 4); packet[0] = 0x46;
    ping_write_be16(packet + 2, (uint16_t)(packet_bytes + 4));
    assert(ping_reply_matches(packet, packet_bytes + 4, payload_bytes, id, sequence));
}

int main(void)
{
    const uint8_t checksum_vector[] = {0x00, 0x01, 0xf2, 0x03, 0xf4, 0xf5, 0xf6, 0xf7};
    const uint8_t odd_vector[] = {0x01, 0x02, 0x03};
    assert(ping_checksum(checksum_vector, sizeof(checksum_vector)) == 0x220d);
    assert(ping_checksum(odd_vector, sizeof(odd_vector)) == 0xfbfd);
    test_size(AI_PING_SMALL_PAYLOAD_BYTES);
    test_size(AI_PING_PAYLOAD_BYTES);
    puts("Ping 32/1472-byte boundary/checksum/content tests PASS");
    return 0;
}

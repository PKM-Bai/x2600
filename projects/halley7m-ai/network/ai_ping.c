#include "ai_ping.h"
#include "ai_ethernet.h"
#include "ping_packet.h"
#include "FreeRTOS.h"
#include "task.h"
#include "lwip/sockets.h"
#include "lwip/inet.h"
#include "lwip/sys.h"
#include <errno.h>
#include <stdio.h>
#include <string.h>

#define PING_COUNT 4u
#define PING_DEADLINE_MS 1000u
static bool running;
static uint16_t next_id;

static bool network_current(uint32_t generation)
{
    ai_ethernet_snapshot snapshot;
    ai_ethernet_snapshot_get(&snapshot);
    return !snapshot.error && snapshot.started && snapshot.link_up &&
           snapshot.address_ready && snapshot.generation == generation;
}

static int ping_gateway(unsigned payload_bytes)
{
    taskENTER_CRITICAL();
    if (running) { taskEXIT_CRITICAL(); return -EBUSY; }
    running = true;
    uint16_t id = ++next_id;
    taskEXIT_CRITICAL();

    int result = 0, socket_fd = -1;
    unsigned sent = 0, received = 0, ignored = 0;
    ai_ethernet_snapshot snapshot;
    ai_ethernet_snapshot_get(&snapshot);
    ip4_addr_t gateway, local;
    if (!network_current(snapshot.generation) ||
        !ip4addr_aton(snapshot.gateway, &gateway) || ip4_addr_isany_val(gateway) ||
        !ip4addr_aton(snapshot.address, &local) || ip4_addr_isany_val(local)) {
        result = -ENETDOWN; goto done;
    }
    socket_fd = lwip_socket(AF_INET, SOCK_RAW, IPPROTO_ICMP);
    if (socket_fd < 0) { result = -errno; goto done; }
    struct sockaddr_in destination = {0}, source = {0};
    destination.sin_len = sizeof(destination); destination.sin_family = AF_INET;
    destination.sin_addr.s_addr = gateway.addr;
    source.sin_len = sizeof(source); source.sin_family = AF_INET;
    source.sin_addr.s_addr = local.addr;
    if (lwip_bind(socket_fd, (const struct sockaddr *)&source, sizeof(source)) < 0) {
        result = -errno; goto done;
    }
    printf("AI_PING start gateway=%s payload=%u count=%u deadline_ms=%u\n",
           snapshot.gateway, payload_bytes, PING_COUNT, PING_DEADLINE_MS);
    uint8_t request[AI_PING_ICMP_BYTES], response[1536];
    size_t request_bytes = 8u + payload_bytes;
    for (uint16_t sequence = 1; sequence <= PING_COUNT; ++sequence) {
        if (!network_current(snapshot.generation)) { result = -ENETDOWN; break; }
        ping_make_request(request, payload_bytes, id, sequence);
        uint32_t started = sys_now();
        ssize_t count = lwip_sendto(socket_fd, request, request_bytes, 0,
                                   (const struct sockaddr *)&destination, sizeof(destination));
        if (count != (ssize_t)request_bytes) {
            result = count < 0 ? -errno : -EIO;
            printf("AI_PING seq=%u send_error=%d\n", sequence, result);
            break;
        }
        ++sent;
        bool matched = false;
        for (;;) {
            if (!network_current(snapshot.generation)) { result = -ENETDOWN; break; }
            uint32_t elapsed = sys_now() - started;
            if (elapsed >= PING_DEADLINE_MS) break;
            uint32_t remaining = PING_DEADLINE_MS - elapsed;
            struct timeval timeout = { (long)(remaining / 1000u), (long)((remaining % 1000u) * 1000u) };
            fd_set readable; FD_ZERO(&readable); FD_SET(socket_fd, &readable);
            int ready = lwip_select(socket_fd + 1, &readable, NULL, NULL, &timeout);
            if (ready < 0) { result = -errno; break; }
            if (!ready) break;
            struct sockaddr_in sender = {0}; socklen_t sender_bytes = sizeof(sender);
            count = lwip_recvfrom(socket_fd, response, sizeof(response), MSG_DONTWAIT,
                                  (struct sockaddr *)&sender, &sender_bytes);
            if (count < 0) {
                if (errno == EWOULDBLOCK || errno == EAGAIN) continue;
                result = -errno; break;
            }
            if (!network_current(snapshot.generation)) { result = -ENETDOWN; break; }
            if (sys_now() - started >= PING_DEADLINE_MS) break;
            if (sender.sin_family != AF_INET || sender.sin_addr.s_addr != gateway.addr ||
                !ping_reply_matches(response, (size_t)count, payload_bytes, id, sequence)) { ++ignored; continue; }
            ++received; matched = true;
            printf("AI_PING seq=%u bytes=%u rtt_ms=%lu\n", sequence, payload_bytes,
                   (unsigned long)(sys_now() - started));
            break;
        }
        if (result) break;
        if (!matched) printf("AI_PING seq=%u timeout_ms=%u\n", sequence, PING_DEADLINE_MS);
        if (sequence < PING_COUNT) vTaskDelay(pdMS_TO_TICKS(200));
    }
    if (!result && received != PING_COUNT) result = -ETIMEDOUT;
done:
    if (socket_fd >= 0) (void)lwip_close(socket_fd);
    printf("AI_PING done gateway=%s payload=%u sent=%u received=%u ignored=%u result=%d\n",
           snapshot.gateway, payload_bytes, sent, received, ignored, result);
    taskENTER_CRITICAL(); running = false; taskEXIT_CRITICAL();
    return result;
}

int ai_ping_gateway(void) { return ping_gateway(AI_PING_PAYLOAD_BYTES); }
int ai_ping_gateway_small(void) { return ping_gateway(AI_PING_SMALL_PAYLOAD_BYTES); }

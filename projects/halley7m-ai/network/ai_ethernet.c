#include "ai_ethernet.h"
#include "ai_lwip_platform.h"
#include "board_entropy.h"
#include "board_gmac.h"
#include "FreeRTOS.h"
#include "task.h"
#include "queue.h"
#include "semphr.h"
#include "lwip/tcpip.h"
#include "lwip/netifapi.h"
#include "lwip/dhcp.h"
#include "lwip/dns.h"
#include "lwip/etharp.h"
#include "lwip/apps/sntp.h"
#include <errno.h>
#include <stdio.h>
#include <string.h>

#define FRAME_BYTES 1536u
typedef struct { uint32_t generation; uint16_t bytes; uint8_t data[FRAME_BYTES]; } queued_frame;
static ai_ethernet_snapshot state;
static ai_gmac_status_t hardware;
static struct netif ethernet;
static QueueHandle_t transmit_queue;
static SemaphoreHandle_t tcpip_ready;
static TaskHandle_t network_task_handle;
/* Each buffer has a single task owner. No frame is held by DMA here. */
static queued_frame output_frame, pending_frame;
static uint8_t received_frame[FRAME_BYTES];
static bool output_pending;
static uint32_t services_generation;
static uint32_t dns_generation;
static bool dns_pending;
static uint32_t next_dns_ms;
static uint32_t local_ipv4;
/* All resolver/SNTP state below is owned by the tcpip task. The token changes
 * on every lookup and stop, including a stop within the same link generation. */
static const char *const ntp_names[2] = { "ntp.aliyun.com", "time.cloudflare.com" };
static uint32_t ntp_generation, ntp_token, next_ntp_dns_ms;
static unsigned ntp_next_server;
static bool ntp_pending, ntp_valid[2];
static ip_addr_t ntp_addresses[2];

static void address_changed(struct netif *interface);

void ai_ethernet_snapshot_get(ai_ethernet_snapshot *out)
{
    taskENTER_CRITICAL(); *out = state; taskEXIT_CRITICAL();
    uint64_t utc;
    out->time_ready = ai_lwip_get_unix_time(&utc);
}
static void copy_ip(char out[16], const ip4_addr_t *address)
{ (void)ip4addr_ntoa_r(address, out, 16); }
static bool lease_ready(void)
{
    return netif_is_up(&ethernet) && netif_is_link_up(&ethernet) &&
           dhcp_supplied_address(&ethernet) &&
           !ip4_addr_isany_val(*netif_ip4_addr(&ethernet));
}
static void arp_metadata(const uint8_t *frame, size_t bytes, bool transmit)
{
    /* Ethernet/IPv4 ARP only. No packet contents or sender data are logged. */
    if (bytes < 42 || frame[12] != 0x08 || frame[13] != 0x06 ||
        frame[14] != 0 || frame[15] != 1 || frame[16] != 0x08 || frame[17] != 0 ||
        frame[18] != 6 || frame[19] != 4 || frame[20] != 0) return;
    if (transmit) {
        if (frame[21] == 2) {
            taskENTER_CRITICAL(); ++state.arp_tx_reply; taskEXIT_CRITICAL();
        }
        return;
    }
    ip4_addr_t target;
    char target_text[16] = {0};
    memcpy(&target.addr, frame + 38, sizeof(target.addr));
    copy_ip(target_text, &target);
    taskENTER_CRITICAL();
    ++state.arp_rx;
    if (frame[21] == 2) ++state.arp_rx_reply;
    if (frame[21] == 1 && state.address_ready && target.addr == local_ipv4)
        ++state.arp_target_us;
    memcpy(state.last_arp_target, target_text, sizeof(target_text));
    taskEXIT_CRITICAL();
}
static err_t output(struct netif *interface, struct pbuf *packet)
{
    /* Called by the tcpip task. The GMAC owner consumes a copy asynchronously. */
    if (!netif_is_link_up(interface)) return ERR_IF;
    if (packet->tot_len > FRAME_BYTES || packet->tot_len < 14) return ERR_BUF;
    output_frame.bytes = packet->tot_len;
    taskENTER_CRITICAL(); output_frame.generation = state.generation; taskEXIT_CRITICAL();
    if (pbuf_copy_partial(packet, output_frame.data, packet->tot_len, 0) != packet->tot_len)
        return ERR_BUF;
    if (xQueueSend(transmit_queue, &output_frame, 0) != pdTRUE) {
        taskENTER_CRITICAL(); ++state.output_drops; taskEXIT_CRITICAL();
        return ERR_MEM;
    }
    return ERR_OK;
}
static void dns_complete(const char *name, const ip_addr_t *address, void *argument)
{
    (void)name;
    uint32_t generation = (uint32_t)(uintptr_t)argument;
    if (generation != dns_generation) return;
    dns_pending = false;
    next_dns_ms = sys_now() + 30000u;
    taskENTER_CRITICAL();
    bool current = generation == state.generation && !state.error &&
                   state.link_up && state.address_ready && lease_ready();
    if (current) {
        state.dns_ready = address != NULL;
        state.dns_result = address ? ERR_OK : ERR_TIMEOUT;
        if (address) copy_ip(state.cloud_address, ip_2_ip4(address));
    }
    taskEXIT_CRITICAL();
    if (current) printf("AI_DNS result=%d host=api.deepseek.com address=%s\n",
                        address ? 0 : ERR_TIMEOUT, address ? state.cloud_address : "unavailable");
}
static void stop_ntp(void)
{
    if (sntp_enabled()) sntp_stop();
    ++ntp_token;
    ntp_pending = false;
    ntp_generation = 0;
    ntp_next_server = 0;
    ntp_valid[0] = ntp_valid[1] = false;
    sntp_setserver(0, NULL); sntp_setserver(1, NULL);
}
static void ntp_dns_complete(const char *name, const ip_addr_t *address, void *argument)
{
    uint32_t token = (uint32_t)(uintptr_t)argument;
    if (token != ntp_token || !ntp_pending) return;
    ntp_pending = false;
    ai_ethernet_snapshot snapshot;
    ai_ethernet_snapshot_get(&snapshot);
    if (snapshot.error || !snapshot.link_up || !snapshot.address_ready || !lease_ready() ||
        snapshot.generation != ntp_generation || services_generation != ntp_generation) {
        stop_ntp(); services_generation = 0; return;
    }
    unsigned server = ntp_next_server++;
    if (address && !ip_addr_isany(address)) {
        ntp_addresses[server] = *address; ntp_valid[server] = true;
        /* Duplicate the first resolved address until both are available. The
         * lwIP client can retain current_server == 1 across stop/start. */
        sntp_setserver(0, &ntp_addresses[ntp_valid[0] ? 0 : 1]);
        sntp_setserver(1, &ntp_addresses[ntp_valid[1] ? 1 : 0]);
        if (!sntp_enabled()) {
            sntp_setoperatingmode(SNTP_OPMODE_POLL); sntp_init();
        }
    }
    printf("AI_NTP_DNS host=%s result=%d\n", name, ntp_valid[server] ? 0 : ERR_TIMEOUT);
    while (ntp_next_server < 2 && ntp_valid[ntp_next_server]) ++ntp_next_server;
    if (ntp_next_server == 2 && (!ntp_valid[0] || !ntp_valid[1])) {
        ntp_next_server = 0; next_ntp_dns_ms = sys_now() + 30000u;
    }
}
static void poll_ntp_dns(void)
{
    if (ntp_pending || (int32_t)(sys_now() - next_ntp_dns_ms) < 0) return;
    while (ntp_next_server < 2 && ntp_valid[ntp_next_server]) ++ntp_next_server;
    if (ntp_next_server == 2) return;
    ip_addr_t address;
    const char *name = ntp_names[ntp_next_server];
    ++ntp_token; ntp_pending = true;
    void *argument = (void *)(uintptr_t)ntp_token;
    err_t result = dns_gethostbyname(name, &address, ntp_dns_complete, argument);
    if (result == ERR_OK) ntp_dns_complete(name, &address, argument);
    else if (result != ERR_INPROGRESS) ntp_dns_complete(name, NULL, argument);
}
static void service_poll(void *unused)
{
    (void)unused;
    /* An ACK that renews the same address need not invoke the netif callback.
     * Recheck DHCP state so REBOOTING never exposes its retained old address. */
    address_changed(&ethernet);
    struct eth_addr *gateway_mac;
    const ip4_addr_t *gateway_ip;
    uint8_t gateway_bytes[6] = {0};
    if (lease_ready() && etharp_find_addr(&ethernet, netif_ip4_gw(&ethernet),
                                        &gateway_mac, &gateway_ip) >= 0)
        memcpy(gateway_bytes, gateway_mac->addr, sizeof(gateway_bytes));
    taskENTER_CRITICAL();
    memcpy(state.gateway_mac, gateway_bytes, sizeof(gateway_bytes));
    taskEXIT_CRITICAL();
    ai_ethernet_snapshot snapshot;
    ai_ethernet_snapshot_get(&snapshot);
    if (snapshot.error || !snapshot.link_up || !snapshot.address_ready) {
        if (ntp_generation || ntp_pending || sntp_enabled()) stop_ntp();
        services_generation = 0;
        return;
    }
    if (services_generation != snapshot.generation) {
        services_generation = snapshot.generation;
        stop_ntp();
        ntp_generation = snapshot.generation;
        next_ntp_dns_ms = sys_now();
        next_dns_ms = sys_now();
    }
    poll_ntp_dns();
    if ((!dns_pending || dns_generation != snapshot.generation) && !snapshot.dns_ready &&
        (int32_t)(sys_now() - next_dns_ms) >= 0) {
        ip_addr_t address;
        dns_generation = snapshot.generation;
        dns_pending = true;
        err_t result = dns_gethostbyname("api.deepseek.com", &address, dns_complete,
                                        (void *)(uintptr_t)dns_generation);
        if (result == ERR_OK) dns_complete("api.deepseek.com", &address, (void *)(uintptr_t)dns_generation);
        else if (result != ERR_INPROGRESS) dns_complete("api.deepseek.com", NULL, (void *)(uintptr_t)dns_generation);
    }
}
static void address_changed(struct netif *interface)
{
    char address[16] = {0}, mask[16] = {0}, gateway[16] = {0}, dns[16] = {0};
    copy_ip(address, netif_ip4_addr(interface)); copy_ip(mask, netif_ip4_netmask(interface));
    copy_ip(gateway, netif_ip4_gw(interface)); copy_ip(dns, ip_2_ip4(dns_getserver(0)));
    bool ready = lease_ready();
    taskENTER_CRITICAL();
    ready = ready && state.link_up && !state.error;
    bool changed = strcmp(state.address, address) || strcmp(state.netmask, mask) ||
                   strcmp(state.gateway, gateway) || strcmp(state.dns_server, dns) ||
                   state.address_ready != ready;
    if (changed) {
        ++state.generation; state.dns_ready = false; state.cloud_address[0] = '\0';
    }
    state.address_ready = ready;
    memcpy(state.address, address, 16); memcpy(state.netmask, mask, 16);
    memcpy(state.gateway, gateway, 16); memcpy(state.dns_server, dns, 16);
    local_ipv4 = netif_ip4_addr(interface)->addr;
    taskEXIT_CRITICAL();
    if (changed) printf("AI_ADDRESS ready=%u ip=%s mask=%s gateway=%s dns=%s\n",
                        ready, address, mask, gateway, dns);
}
static err_t ethernet_netif_init(struct netif *interface)
{
    interface->name[0] = 'e'; interface->name[1] = 'n';
    interface->hwaddr_len = 6; memcpy(interface->hwaddr, state.mac, 6);
    interface->mtu = 1500; interface->flags = NETIF_FLAG_BROADCAST | NETIF_FLAG_ETHARP;
    interface->output = etharp_output; interface->linkoutput = output;
    netif_set_status_callback(interface, address_changed);
    return ERR_OK;
}
static void tcpip_initialized(void *unused)
{ (void)unused; (void)xSemaphoreGive(tcpip_ready); }
static void fail_network(int error)
{
    taskENTER_CRITICAL();
    state.error = error; state.link_up = false;
    state.address_ready = false; state.dns_ready = false; ++state.generation;
    taskEXIT_CRITICAL();
    printf("AI_ETH error=%d\n", error);
}
static void network_task(void *unused)
{
    (void)unused;
    int result = ai_board_entropy_init();
    if (result) { fail_network(result); vTaskDelete(NULL); return; }
    ai_lwip_set_random_source(ai_board_entropy_lwip, NULL);
    uint8_t mac[6];
    if (!ai_board_entropy_lwip(NULL, mac, sizeof(mac))) {
        fail_network(-EIO); vTaskDelete(NULL); return;
    }
    /* Prototype MAC: random locally administered unicast, new on each boot. */
    mac[0] = (uint8_t)((mac[0] & 0xfcu) | 2u);
    taskENTER_CRITICAL(); memcpy(state.mac, mac, sizeof(mac)); taskEXIT_CRITICAL();
    result = ai_board_gmac_init(mac);
    ai_gmac_status_t initial; ai_board_gmac_status(&initial);
    taskENTER_CRITICAL(); hardware = initial; taskEXIT_CRITICAL();
    if (result) { fail_network(result); vTaskDelete(NULL); return; }
    tcpip_init(tcpip_initialized, NULL);
    configASSERT(xSemaphoreTake(tcpip_ready, pdMS_TO_TICKS(5000)) == pdTRUE);
    vSemaphoreDelete(tcpip_ready); tcpip_ready = NULL;
    ip4_addr_t zero; ip4_addr_set_zero(&zero);
    err_t error = netifapi_netif_add(&ethernet, &zero, &zero, &zero, NULL, ethernet_netif_init, tcpip_input);
    if (!error) error = netifapi_netif_set_default(&ethernet);
    if (!error) error = netifapi_netif_set_up(&ethernet);
    if (!error) error = netifapi_dhcp_start(&ethernet);
    if (error) { (void)ai_board_gmac_stop(); fail_network(error); vTaskDelete(NULL); return; }
    printf("AI_ETH started mac=%02x:%02x:%02x:%02x:%02x:%02x mode=dhcp\n",
           state.mac[0], state.mac[1], state.mac[2], state.mac[3], state.mac[4], state.mac[5]);
    taskENTER_CRITICAL(); state.started = true; taskEXIT_CRITICAL();
    uint32_t next_link = 0, next_service = 0;
    bool link = false;
    for (;;) {
        uint32_t now = sys_now();
        if ((int32_t)(now - next_link) >= 0) {
            next_link = now + 250;
            result = ai_board_gmac_poll_link();
            ai_gmac_status_t current; ai_board_gmac_status(&current);
            taskENTER_CRITICAL(); hardware = current; state.speed_mbps = current.speed_mbps; taskEXIT_CRITICAL();
            if (result < 0) {
                (void)netifapi_netif_set_link_down(&ethernet);
                (void)ai_board_gmac_stop(); fail_network(result); vTaskDelete(NULL); return;
            }
            if (link != current.link_up) {
                link = current.link_up;
                taskENTER_CRITICAL();
                state.link_up = link; state.address_ready = false;
                state.dns_ready = false; ++state.generation;
                taskEXIT_CRITICAL();
                output_pending = false; (void)xQueueReset(transmit_queue);
                if (link) (void)netifapi_netif_set_link_up(&ethernet);
                else (void)netifapi_netif_set_link_down(&ethernet);
                printf("AI_LINK up=%u speed=%u duplex=%u\n", link, current.speed_mbps, current.full_duplex);
            }
        }
        for (unsigned i = 0; i < 16; ++i) {
            int bytes = ai_board_gmac_read(received_frame, sizeof(received_frame));
            if (!bytes) break;
            if (bytes < 0) { taskENTER_CRITICAL(); ++state.input_drops; taskEXIT_CRITICAL(); break; }
            arp_metadata(received_frame, (size_t)bytes, false);
            struct pbuf *packet = pbuf_alloc(PBUF_RAW, (u16_t)bytes, PBUF_POOL);
            if (!packet || pbuf_take(packet, received_frame, (u16_t)bytes) != ERR_OK ||
                ethernet.input(packet, &ethernet) != ERR_OK) {
                if (packet) pbuf_free(packet);
                taskENTER_CRITICAL(); ++state.input_drops; taskEXIT_CRITICAL();
            } else { taskENTER_CRITICAL(); ++state.received; taskEXIT_CRITICAL(); }
        }
        for (unsigned i = 0; i < 8 && link; ++i) {
            if (!output_pending) output_pending = xQueueReceive(transmit_queue, &pending_frame, 0) == pdTRUE;
            if (!output_pending) break;
            if (pending_frame.generation != state.generation) { output_pending = false; continue; }
            result = ai_board_gmac_send(pending_frame.data, pending_frame.bytes);
            if (result == -EAGAIN) break;
            output_pending = false;
            if (!result) arp_metadata(pending_frame.data, pending_frame.bytes, true);
            taskENTER_CRITICAL();
            if (!result) ++state.submitted; else ++state.output_drops;
            taskEXIT_CRITICAL();
        }
        if ((int32_t)(now - next_service) >= 0) {
            next_service = now + 1000;
            (void)tcpip_try_callback(service_poll, NULL);
        }
        vTaskDelay(pdMS_TO_TICKS(1));
    }
}
int ai_ethernet_start(void)
{
    if (network_task_handle) return -EBUSY;
    transmit_queue = xQueueCreate(8, sizeof(queued_frame));
    tcpip_ready = xSemaphoreCreateBinary();
    if (!transmit_queue || !tcpip_ready) {
        if (transmit_queue) vQueueDelete(transmit_queue);
        if (tcpip_ready) vSemaphoreDelete(tcpip_ready);
        transmit_queue = NULL; tcpip_ready = NULL;
        fail_network(-ENOMEM);
        return -ENOMEM;
    }
    if (xTaskCreate(network_task, "ai_net", 4096, NULL, 3, &network_task_handle) != pdPASS) {
        vQueueDelete(transmit_queue); vSemaphoreDelete(tcpip_ready);
        transmit_queue = NULL; tcpip_ready = NULL;
        fail_network(-ENOMEM); return -ENOMEM;
    }
    return 0;
}
void ai_ethernet_print_status(void)
{
    ai_ethernet_snapshot snapshot; ai_ethernet_snapshot_get(&snapshot);
    ai_gmac_status_t current;
    taskENTER_CRITICAL(); current = hardware; taskEXIT_CRITICAL();
    uint64_t utc = 0; (void)ai_lwip_get_unix_time(&utc);
    printf("AI_NET started=%u error=%d link=%u speed=%u address_ready=%u dns_ready=%u utc_ready=%u generation=%lu\n",
           snapshot.started, snapshot.error, snapshot.link_up, snapshot.speed_mbps,
           snapshot.address_ready, snapshot.dns_ready, snapshot.time_ready, (unsigned long)snapshot.generation);
    printf("AI_NET ip=%s mask=%s gateway=%s dns=%s cloud_ip=%s utc=%llu\n", snapshot.address,
           snapshot.netmask, snapshot.gateway, snapshot.dns_server, snapshot.cloud_address, (unsigned long long)utc);
    printf("AI_NET rx=%lu tx_submit=%lu input_drop=%lu output_drop=%lu stack_free_words=%lu\n",
           (unsigned long)snapshot.received, (unsigned long)snapshot.submitted, (unsigned long)snapshot.input_drops,
           (unsigned long)snapshot.output_drops,
           network_task_handle && !snapshot.error ? (unsigned long)uxTaskGetStackHighWaterMark(network_task_handle) : 0ul);
    printf("AI_ARP rx=%lu target_us=%lu rx_reply=%lu tx_reply_submit=%lu last_target=%s\n",
           (unsigned long)snapshot.arp_rx, (unsigned long)snapshot.arp_target_us,
           (unsigned long)snapshot.arp_rx_reply, (unsigned long)snapshot.arp_tx_reply,
           snapshot.last_arp_target);
    printf("AI_ARP gateway_mac=%02x:%02x:%02x:%02x:%02x:%02x\n", snapshot.gateway_mac[0],
           snapshot.gateway_mac[1], snapshot.gateway_mac[2], snapshot.gateway_mac[3],
           snapshot.gateway_mac[4], snapshot.gateway_mac[5]);
    /* Detailed hardware report is filled only by the network owner. */
    printf("AI_NET phy_link=%u phy_speed=%u phy_duplex=%u\n", current.link_up, current.speed_mbps, current.full_duplex);
    printf("AI_GMAC init=%u error=%d phy=%u id=%08lx bmsr=%04x anar=%04x partner=%04x mode=%08lx features=%08lx\n",
           current.initialized, current.last_error, current.phy_address, (unsigned long)current.phy_id,
           current.bmsr, current.advertisement, current.partner, (unsigned long)current.phy_mode,
           (unsigned long)current.hardware_features);
    printf("AI_GMAC rx=%lu tx=%lu rx_err=%lu tx_err=%lu rx_drop=%lu tx_drop=%lu dma=%08lx rx_desc=%08lx tx_desc=%08lx\n",
           (unsigned long)current.rx_packets, (unsigned long)current.tx_packets,
           (unsigned long)current.rx_errors, (unsigned long)current.tx_errors,
           (unsigned long)current.rx_dropped, (unsigned long)current.tx_dropped,
           (unsigned long)current.dma_status, (unsigned long)current.last_rx_descriptor,
           (unsigned long)current.last_tx_descriptor);
    const ai_gmac_mmc_t *mmc = &current.mmc;
    printf("AI_MMC available=%u control=%08lx read_clear=%u frozen=%u cumulative=1\n",
           mmc->available, (unsigned long)mmc->control,
           (mmc->control & 4u) != 0, (mmc->control & 8u) != 0);
    if (mmc->available) {
        printf("AI_MMC_TX frames_gb=%lu frames_good=%lu octets_gb=%lu size1024_max=%lu underflow=%lu\n",
               (unsigned long)mmc->tx_frames_gb, (unsigned long)mmc->tx_frames_good,
               (unsigned long)mmc->tx_octets_gb, (unsigned long)mmc->tx_1024_max,
               (unsigned long)mmc->tx_underflow);
        printf("AI_MMC_RX frames_gb=%lu octets_gb=%lu octets_good=%lu size1024_max=%lu\n",
               (unsigned long)mmc->rx_frames_gb, (unsigned long)mmc->rx_octets_gb,
               (unsigned long)mmc->rx_octets_good, (unsigned long)mmc->rx_1024_max);
        printf("AI_MMC_RX_ERR crc=%lu align=%lu runt=%lu jabber=%lu oversize_good=%lu overflow=%lu watchdog=%lu\n",
               (unsigned long)mmc->rx_crc, (unsigned long)mmc->rx_align,
               (unsigned long)mmc->rx_runt, (unsigned long)mmc->rx_jabber,
               (unsigned long)mmc->rx_oversize_good, (unsigned long)mmc->rx_overflow,
               (unsigned long)mmc->rx_watchdog);
    }
}

#include "ai_network.h"
#include <assert.h>
#include <stdio.h>

int main(void)
{
    ai_network_t network;
    ai_net_path_t ready = {true, true, true, true};
    ai_net_path_t link_only = {true, true, false, false};
    ai_network_init(&network);
    assert(network.selected == AI_NET_NONE && network.generation == 0);
    assert(ai_network_probe_target(&network) == AI_NET_NONE);
    assert(!ai_network_update(&network, AI_NET_ETHERNET, link_only));
    assert(ai_network_update(&network, AI_NET_WIFI, ready));
    assert(network.selected == AI_NET_WIFI && network.generation == 1);
    link_only.address_ready = true;
    assert(!ai_network_update(&network, AI_NET_ETHERNET, link_only));
    assert(ai_network_probe_target(&network) == AI_NET_ETHERNET);
    assert(ai_network_update(&network, AI_NET_ETHERNET, ready));
    assert(network.selected == AI_NET_ETHERNET && network.generation == 2);
    assert(!ai_network_update(&network, AI_NET_ETHERNET, ready));
    assert(!ai_network_failure(&network, AI_NET_ETHERNET, AI_NET_FAILURE_AUTH));
    assert(!ai_network_failure(&network, AI_NET_ETHERNET, AI_NET_FAILURE_QUOTA));
    assert(!ai_network_failure(&network, AI_NET_ETHERNET, AI_NET_FAILURE_PROVIDER));
    assert(network.selected == AI_NET_ETHERNET && network.generation == 2);
    assert(ai_network_failure(&network, AI_NET_ETHERNET, AI_NET_FAILURE_TRANSPORT));
    assert(network.selected == AI_NET_WIFI && network.generation == 3);
    ready.link_up = false; /* Stale address/cloud flags must be discarded. */
    assert(ai_network_update(&network, AI_NET_WIFI, ready));
    assert(network.selected == AI_NET_NONE && network.generation == 4);
    assert(!network.paths[AI_NET_WIFI].address_ready);
    assert(!network.paths[AI_NET_WIFI].cloud_reachable);
    ready.link_up = true;
    ready.driver_ready = false;
    assert(!ai_network_update(&network, AI_NET_WIFI, ready));
    assert(!network.paths[AI_NET_WIFI].link_up);
    assert(!ai_network_update(&network, AI_NET_NONE, ready));
    assert(!ai_network_failure(&network, AI_NET_COUNT, AI_NET_FAILURE_TRANSPORT));
    /* A failed Ethernet probe must not starve Wi-Fi reachability discovery. */
    ai_network_init(&network);
    ready.driver_ready = true;
    ready.cloud_reachable = false;
    assert(!ai_network_update(&network, AI_NET_ETHERNET, ready));
    assert(!ai_network_update(&network, AI_NET_WIFI, ready));
    assert(ai_network_probe_target(&network) == AI_NET_ETHERNET);
    assert(ai_network_probe_target(&network) == AI_NET_WIFI);
    assert(ai_network_probe_target(&network) == AI_NET_ETHERNET);
    /* Restored Ethernet must not interrupt a healthy active Wi-Fi request. */
    ai_network_init(&network);
    ready.cloud_reachable = true;
    assert(ai_network_update(&network, AI_NET_WIFI, ready));
    assert(!ai_network_set_request_active(&network, true));
    assert(!ai_network_update(&network, AI_NET_ETHERNET, ready));
    assert(network.selected == AI_NET_WIFI && network.generation == 1);
    assert(!ai_network_failure(&network, AI_NET_WIFI, AI_NET_FAILURE_AUTH));
    assert(ai_network_set_request_active(&network, false));
    assert(network.selected == AI_NET_ETHERNET && network.generation == 2);
    assert(ai_network_failure(&network, AI_NET_ETHERNET, AI_NET_FAILURE_TRANSPORT));
    assert(!ai_network_set_request_active(&network, true));
    assert(!ai_network_update(&network, AI_NET_ETHERNET, ready));
    assert(ai_network_failure(&network, AI_NET_WIFI, AI_NET_FAILURE_TRANSPORT));
    assert(network.selected == AI_NET_ETHERNET && network.generation == 4);
    /* Physical loss likewise overrides the active request hold. */
    assert(ai_network_failure(&network, AI_NET_ETHERNET, AI_NET_FAILURE_TRANSPORT));
    assert(ai_network_update(&network, AI_NET_WIFI, ready));
    assert(!ai_network_update(&network, AI_NET_ETHERNET, ready));
    ready.link_up = false;
    assert(ai_network_update(&network, AI_NET_WIFI, ready));
    assert(network.selected == AI_NET_ETHERNET);
    puts("network_host_test: PASS (priority, active request, reachability, failures, stale state)");
    return 0;
}

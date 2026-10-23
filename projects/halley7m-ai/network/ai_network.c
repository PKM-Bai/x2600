#include "ai_network.h"
#include <string.h>

static bool addressed(const ai_net_path_t *path)
{
    return path->driver_ready && path->link_up && path->address_ready;
}

static bool select_path(ai_network_t *network)
{
    ai_net_id_t selected = AI_NET_NONE;
    if (network->request_active && network->selected == AI_NET_WIFI &&
        addressed(&network->paths[AI_NET_WIFI]) &&
        network->paths[AI_NET_WIFI].cloud_reachable)
        return false;
    for (ai_net_id_t id = AI_NET_ETHERNET; id < AI_NET_COUNT; ++id) {
        if (addressed(&network->paths[id]) &&
            network->paths[id].cloud_reachable) {
            selected = id;
            break;
        }
    }
    if (selected == network->selected)
        return false;
    network->selected = selected;
    ++network->generation;
    return true;
}

void ai_network_init(ai_network_t *network)
{
    memset(network, 0, sizeof(*network));
    network->next_probe = AI_NET_ETHERNET;
}

bool ai_network_set_request_active(ai_network_t *network, bool active)
{
    network->request_active = active;
    return select_path(network);
}

bool ai_network_update(ai_network_t *network, ai_net_id_t id,
                       ai_net_path_t path)
{
    if (id <= AI_NET_NONE || id >= AI_NET_COUNT)
        return false;
    if (!path.driver_ready)
        path.link_up = false;
    if (!path.link_up)
        path.address_ready = false;
    if (!path.address_ready)
        path.cloud_reachable = false;
    network->paths[id] = path;
    return select_path(network);
}

bool ai_network_failure(ai_network_t *network, ai_net_id_t id,
                        ai_net_failure_t reason)
{
    if (id <= AI_NET_NONE || id >= AI_NET_COUNT ||
        reason != AI_NET_FAILURE_TRANSPORT)
        return false;
    network->paths[id].cloud_reachable = false;
    return select_path(network);
}

ai_net_id_t ai_network_probe_target(ai_network_t *network)
{
    for (unsigned attempt = 0; attempt < AI_NET_COUNT - 1; ++attempt) {
        ai_net_id_t id = network->next_probe;
        network->next_probe = id == AI_NET_ETHERNET ? AI_NET_WIFI : AI_NET_ETHERNET;
        if (addressed(&network->paths[id]) &&
            !network->paths[id].cloud_reachable)
            return id;
    }
    return AI_NET_NONE;
}

const char *ai_network_name(ai_net_id_t id)
{
    switch (id) {
    case AI_NET_ETHERNET: return "ethernet";
    case AI_NET_WIFI: return "wifi";
    default: return "none";
    }
}

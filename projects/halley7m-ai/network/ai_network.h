#ifndef AI_NETWORK_H
#define AI_NETWORK_H

#include <stdbool.h>
#include <stdint.h>

typedef enum {
    AI_NET_NONE = 0,
    AI_NET_ETHERNET,
    AI_NET_WIFI,
    AI_NET_COUNT
} ai_net_id_t;

typedef struct {
    bool driver_ready;
    bool link_up;
    bool address_ready;
    bool cloud_reachable;
} ai_net_path_t;

typedef enum {
    AI_NET_FAILURE_TRANSPORT,
    AI_NET_FAILURE_AUTH,
    AI_NET_FAILURE_QUOTA,
    AI_NET_FAILURE_PROVIDER
} ai_net_failure_t;

/* Owned by one network task. Publish snapshots to other tasks by queue. */
typedef struct {
    ai_net_path_t paths[AI_NET_COUNT];
    ai_net_id_t selected;
    ai_net_id_t next_probe;
    uint32_t generation;
    bool request_active;
} ai_network_t;

void ai_network_init(ai_network_t *network);
/* A healthy Wi-Fi request keeps its path until completion. Loss of its path
 * still switches immediately. Returns true if releasing a request switches. */
bool ai_network_set_request_active(ai_network_t *network, bool active);
/* Returns true only when the selected path changes. A change invalidates an
 * existing socket/request; the caller retains its payload for manual retry. */
bool ai_network_update(ai_network_t *network, ai_net_id_t id,
                       ai_net_path_t path);
bool ai_network_failure(ai_network_t *network, ai_net_id_t id,
                        ai_net_failure_t reason);
/* Chooses an addressed path for a reachability probe, not for sending a query.
 * A successful, verified probe must be reported through ai_network_update. */
ai_net_id_t ai_network_probe_target(ai_network_t *network);
const char *ai_network_name(ai_net_id_t id);

#endif

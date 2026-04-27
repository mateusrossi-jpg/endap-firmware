#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define NODE_REGISTRY_MAX_NODES 10
#define NODE_REGISTRY_PROFILE_LEN 24
#define NODE_REGISTRY_TEMPLATE_LEN 24

typedef enum
{
    NODE_REGISTRY_TRANSPORT_NONE = 0,
    NODE_REGISTRY_TRANSPORT_WIFI_UDP = 1,
    NODE_REGISTRY_TRANSPORT_ETHERNET_UDP = 2,
    NODE_REGISTRY_TRANSPORT_RS485_CLUSTER = 3,
    NODE_REGISTRY_TRANSPORT_RS485_RUNTIME = NODE_REGISTRY_TRANSPORT_RS485_CLUSTER,
} node_registry_transport_t;

typedef enum
{
    NODE_REGISTRY_OFFLINE_NONE = 0,
    NODE_REGISTRY_OFFLINE_HEARTBEAT_TIMEOUT = 1,
    NODE_REGISTRY_OFFLINE_MISSING_ADDRESS = 2,
    NODE_REGISTRY_OFFLINE_LINK_DEGRADED = 3,
} node_registry_offline_reason_t;

enum
{
    NODE_REGISTRY_RECOVERY_TRY_RECONNECT = 1 << 0,
    NODE_REGISTRY_RECOVERY_REENABLE_WIFI = 1 << 1,
    NODE_REGISTRY_RECOVERY_FORCE_MODE = 1 << 2,
    NODE_REGISTRY_RECOVERY_IDENTIFY = 1 << 3,
};

typedef enum
{
    NODE_REGISTRY_STATE_DISCOVERED = 0,
    NODE_REGISTRY_STATE_ADOPTED,
    NODE_REGISTRY_STATE_CONFIGURED,
    NODE_REGISTRY_STATE_ACTIVE,
} node_registry_state_t;

typedef struct
{
    uint32_t node_id;
    uint32_t last_ip_addr;
    uint32_t age_ms;
    uint32_t last_seen_ms;
    uint8_t health;
    uint8_t cluster_state;
    uint8_t registry_state;
    uint8_t last_transport;
    uint8_t offline_reason;
    uint8_t recovery_capabilities;
    uint8_t reserved0;
    uint8_t reserved1;
    char profile[NODE_REGISTRY_PROFILE_LEN];
    char template_name[NODE_REGISTRY_TEMPLATE_LEN];
} node_registry_entry_t;

void node_registry_init(void);
void node_registry_process(void);

int node_registry_export(node_registry_entry_t *out_entries, int max_entries);
bool node_registry_note_transport(uint32_t node_id, uint8_t transport, uint32_t source_ip);

node_registry_state_t node_registry_get_state(uint32_t node_id);
bool node_registry_is_known(uint32_t node_id);
bool node_registry_is_adopted(uint32_t node_id);
bool node_registry_is_operational(uint32_t node_id);

bool node_registry_adopt(uint32_t node_id);
bool node_registry_configure(uint32_t node_id, const char *profile, const char *template_name);
bool node_registry_activate(uint32_t node_id);
bool node_registry_revoke(uint32_t node_id);

const char *node_registry_state_name(node_registry_state_t state);
const char *node_registry_cluster_state_name(uint8_t cluster_state);
const char *node_registry_transport_name(uint8_t transport);
const char *node_registry_offline_reason_name(uint8_t reason);

#ifdef __cplusplus
}
#endif

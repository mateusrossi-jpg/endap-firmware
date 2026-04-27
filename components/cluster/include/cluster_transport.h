#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define CLUSTER_TRANSPORT_FRAME_PORT     5000
#define CLUSTER_TRANSPORT_DISCOVERY_PORT 5005

typedef enum
{
    CLUSTER_TRANSPORT_NONE = 0,
    CLUSTER_TRANSPORT_WIFI_UDP = 1,
    CLUSTER_TRANSPORT_ETHERNET_UDP = 2,
    CLUSTER_TRANSPORT_RS485 = 3,
} cluster_transport_type_t;

typedef struct
{
    uint32_t node_id;
    uint32_t timestamp_ms;
    uint32_t source_ip;
    uint8_t source_transport;
} cluster_transport_heartbeat_t;

typedef void (*cluster_transport_heartbeat_cb_t)(const cluster_transport_heartbeat_t *heartbeat);
typedef void (*cluster_transport_frame_cb_t)(const uint8_t *data, uint16_t len);

bool cluster_transport_start(uint32_t self_node_id, cluster_transport_type_t type);
bool cluster_transport_is_ready(void);
cluster_transport_type_t cluster_transport_active_type(void);
const char *cluster_transport_name(cluster_transport_type_t type);
const char *cluster_transport_active_name(void);
void cluster_transport_set_active_type(cluster_transport_type_t type);

void cluster_transport_register_heartbeat_callback(cluster_transport_heartbeat_cb_t cb);
void cluster_transport_register_frame_callback(cluster_transport_frame_cb_t cb);

bool cluster_transport_send_heartbeat(void);
bool cluster_transport_broadcast_frame(const uint8_t *data, uint16_t len);

#ifdef __cplusplus
}
#endif

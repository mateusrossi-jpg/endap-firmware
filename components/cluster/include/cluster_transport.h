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
    CLUSTER_TRANSPORT_WIFI_MESH = 4,
    CLUSTER_TRANSPORT_WIFI_NOW = 5,
} cluster_transport_type_t;

#define ENDAP_CLUSTER_MAGIC   0x454E4450
#define ENDAP_CLUSTER_VERSION 1

typedef struct __attribute__((packed))
{
    uint32_t magic;
    uint32_t node_id;
    uint32_t boot_counter;
    uint32_t uptime_seconds;
    uint8_t node_type;
    uint8_t state;
} endap_heartbeat_t;

typedef struct
{
    uint32_t node_id;
    uint32_t timestamp_ms;
    uint32_t source_ip;
    uint8_t source_transport;
    uint8_t device_profile;
    uint16_t flags;
    uint32_t uptime_seconds;
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

#define ENDAP_REMOTE_CLAIM_MAGIC 0x434C4D31U

typedef enum {
    ENDAP_REMOTE_CLAIM_REQ = 0x01,
    ENDAP_REMOTE_CLAIM_RESP = 0x02
} endap_remote_claim_type_t;

typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint8_t  msg_type;
    uint8_t  profile;
    uint16_t status_code;
    uint32_t target_node_id;
    uint32_t gateway_id;
    char     node_name[32];
} endap_remote_claim_msg_t;

bool cluster_transport_send_remote_claim(uint32_t target_node_id, uint8_t profile, uint32_t gateway_id, const char *node_name, uint32_t timeout_ms);

#define ENDAP_REMOTE_TRANSPORT_MAGIC 0x54525031U

typedef enum {
    ENDAP_REMOTE_TRANSPORT_SET = 0x01,
    ENDAP_REMOTE_TRANSPORT_ACK = 0x02
} endap_remote_transport_type_t;

typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint8_t  msg_type;
    uint8_t  primary_transport;   /* device_profile_transport_t values */
    uint8_t  fallback_transport;  /* device_profile_transport_t values */
    uint8_t  wifi_mode;           /* device_profile_wifi_mode_t */
    uint8_t  flags;               /* bit0=wifi bit1=eth bit2=rs485 */
    uint16_t status_code;
    uint32_t target_node_id;
    uint32_t gateway_id;
} endap_remote_transport_msg_t;

bool cluster_transport_send_remote_transport_set(
    uint32_t target_node_id,
    uint8_t primary, uint8_t fallback, uint8_t wifi_mode, uint8_t flags,
    uint32_t gateway_id, uint32_t timeout_ms);

#ifdef __cplusplus
}
#endif

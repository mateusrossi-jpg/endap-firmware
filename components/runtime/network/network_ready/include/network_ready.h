#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum
{
    NETWORK_READY_LINK_NONE = 0,
    NETWORK_READY_LINK_WIFI_AP = 1,
    NETWORK_READY_LINK_WIFI_STA = 2,
    NETWORK_READY_LINK_ETHERNET = 3,
} network_ready_link_t;

typedef struct
{
    bool ready;
    network_ready_link_t active_link;
    uint32_t active_ip_addr;
    uint32_t active_netmask_addr;
    bool wifi_ap_up;
    bool wifi_sta_up;
    bool ethernet_up;
    uint32_t wifi_ap_ip_addr;
    uint32_t wifi_ap_netmask_addr;
    uint32_t wifi_sta_ip_addr;
    uint32_t wifi_sta_netmask_addr;
    uint32_t ethernet_ip_addr;
    uint32_t ethernet_netmask_addr;
} network_ready_snapshot_t;

typedef void (*network_ready_callback_t)(const network_ready_snapshot_t *snapshot, void *ctx);

void network_ready_init(void);
bool network_ready_register_callback(network_ready_callback_t cb, void *ctx);
bool network_ready_publish_up(network_ready_link_t link, uint32_t ip_addr, uint32_t netmask_addr);
bool network_ready_publish_down(network_ready_link_t link);
bool network_ready_is_up(void);
network_ready_link_t network_ready_active_link(void);
const char *network_ready_active_name(void);
const char *network_ready_link_name(network_ready_link_t link);
void network_ready_get_snapshot(network_ready_snapshot_t *out_snapshot);

#ifdef __cplusplus
}
#endif

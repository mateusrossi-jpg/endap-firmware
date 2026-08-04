#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include "cluster_transport.h"

#ifdef __cplusplus
extern "C" {
#endif

bool cluster_transport_now_init(uint32_t self_node_id);
void cluster_transport_now_deinit(void);

bool cluster_transport_now_send(const uint8_t *data, size_t len);
void cluster_transport_now_register_callbacks(cluster_transport_heartbeat_cb_t hb_cb, cluster_transport_frame_cb_t frame_cb);

typedef struct {
    uint32_t tx_packets;
    uint32_t rx_packets;
    uint32_t tx_errors;
    uint32_t rx_errors;
    uint32_t last_tx_ms;
    uint32_t last_rx_ms;
    uint32_t peers_count;
} cluster_now_stats_t;

void cluster_transport_now_get_stats(cluster_now_stats_t *stats);

#ifdef __cplusplus
}
#endif

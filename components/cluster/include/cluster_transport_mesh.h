#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include "cluster_transport.h"

#ifdef __cplusplus
extern "C" {
#endif

bool cluster_transport_mesh_init(uint32_t self_node_id);
void cluster_transport_mesh_deinit(void);

bool cluster_transport_mesh_send(const uint8_t *data, size_t len);
void cluster_transport_mesh_register_callbacks(cluster_transport_heartbeat_cb_t hb_cb, cluster_transport_frame_cb_t frame_cb);

#ifdef __cplusplus
}
#endif

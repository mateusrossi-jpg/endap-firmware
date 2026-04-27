#pragma once

#include <stdint.h>
#include "cluster_metrics.h"

#ifdef __cplusplus
extern "C" {
#endif

#define MAX_NODES 10

typedef enum {
    CLUSTER_NODE_ONLINE = 0,
    CLUSTER_NODE_SUSPECT,
    CLUSTER_NODE_OFFLINE
} cluster_node_state_t;

typedef struct {
    uint32_t node_id;
    uint32_t ip;

    uint32_t last_seen_ms;
    uint32_t age_ms;

    cluster_node_state_t state;

    uint32_t missed_heartbeats;
    uint32_t recoveries;

    uint8_t health; // 0-100%
} cluster_node_t;

/* Init */
void cluster_manager_start(uint32_t self_node_id);
void cluster_manager_process(void);

/* Update via discovery */
void cluster_manager_update_node(uint32_t node_id, uint32_t ip);
void cluster_manager_remove_node(uint32_t node_id);

/* Metrics */
cluster_metrics_t cluster_get_metrics(void);
int cluster_manager_export_nodes(cluster_node_t *out_nodes, int max_nodes);

/**
 * Retorna o node master atual (menor node_id ONLINE)
 * Usado para evitar split-brain
 */
uint32_t cluster_get_master_node(void);

#ifdef __cplusplus
}
#endif

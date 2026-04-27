#pragma once
#include <stdint.h>

typedef struct {
    uint32_t active;
    uint32_t total_nodes;
    uint32_t online;
    uint32_t suspect;
    uint32_t offline;
    uint32_t avg_health;
    uint32_t self_node;
    uint32_t master_node;
} cluster_metrics_t;

cluster_metrics_t cluster_get_metrics(void);

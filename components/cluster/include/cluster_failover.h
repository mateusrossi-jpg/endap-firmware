#pragma once

#include <stdint.h>

void cluster_failover_init(uint32_t self_node_id);
void cluster_failover_handle_offline(uint32_t node_id);

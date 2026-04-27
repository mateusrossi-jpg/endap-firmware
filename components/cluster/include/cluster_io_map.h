#pragma once

#include <stdint.h>
#include <stdbool.h>

#define MAX_IO 32

typedef struct {
    uint32_t owner_node;
    bool active;
} io_owner_t;

void cluster_io_init(uint32_t self_id);

void cluster_io_assign(uint8_t io, uint32_t node_id);

bool cluster_io_is_local(uint8_t io);

void cluster_io_failover(uint32_t failed_node, uint32_t self_id);

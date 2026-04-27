#include "cluster_io_map.h"
#include <string.h>

static io_owner_t io_map[MAX_IO];
static uint32_t my_node_id = 0;

void cluster_io_init(uint32_t self_id)
{
    my_node_id = self_id;

    for (int i = 0; i < MAX_IO; i++) {
        io_map[i].owner_node = self_id;
        io_map[i].active = true;
    }
}

void cluster_io_assign(uint8_t io, uint32_t node_id)
{
    if (io >= MAX_IO) return;

    io_map[io].owner_node = node_id;
}

bool cluster_io_is_local(uint8_t io)
{
    if (io >= MAX_IO) return false;

    return (io_map[io].owner_node == my_node_id);
}

void cluster_io_failover(uint32_t failed_node, uint32_t self_id)
{
    for (int i = 0; i < MAX_IO; i++) {
        if (io_map[i].owner_node == failed_node) {
            io_map[i].owner_node = self_id;
        }
    }
}

#pragma once

#include <stdint.h>

#define MAX_IO 64

typedef struct
{
    uint32_t io_id;
    uint32_t owner;
    uint32_t original_owner;
    uint8_t  valid;

} cluster_io_entry_t;

/* Init */
void cluster_io_init(uint32_t self_id);
void cluster_io_register_local(uint32_t io_id);
void cluster_io_sync_all(void);
void cluster_io_set_owner(uint32_t io_id, uint32_t owner, uint32_t original_owner);

/* Failover */
void cluster_io_handle_node_offline(uint32_t failed_node);

/* Failback */
void cluster_io_handle_node_online(uint32_t node_id);

/* Query */
int cluster_io_is_local(uint32_t io_id);
uint32_t cluster_io_get_owner(uint32_t io_id);
uint32_t cluster_io_get_original_owner(uint32_t io_id);

/* Debug */
void cluster_io_dump(void);

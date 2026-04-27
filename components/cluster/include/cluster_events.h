#pragma once

#include <stdint.h>

typedef enum
{
    EVENT_NODE_ONLINE,
    EVENT_NODE_SUSPECT,
    EVENT_NODE_OFFLINE
} cluster_event_type_t;

typedef struct
{
    uint32_t node_id;
    cluster_event_type_t type;
} cluster_event_t;

/* Handler */
typedef void (*cluster_event_handler_t)(cluster_event_t *evt);

/* API */
void cluster_events_subscribe(cluster_event_handler_t handler);
void cluster_publish_event(cluster_event_t *evt);

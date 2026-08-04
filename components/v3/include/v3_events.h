#ifndef V3_EVENTS_H
#define V3_EVENTS_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "v3_identity.h"

#define V3_EVENTS_QUEUE_SIZE 256
#define V3_EVENTS_MASK       (V3_EVENTS_QUEUE_SIZE - 1)

typedef enum {
    RESOURCE_EVT_CREATED = 0,
    RESOURCE_EVT_ONLINE,
    RESOURCE_EVT_OFFLINE,
    RESOURCE_EVT_LOST,
    RESOURCE_EVT_REMOVED
} v3_event_type_t;

typedef struct {
    resource_id_t   resource_id;
    uint32_t        timestamp_epoch;
    uint8_t         type;
    uint8_t         padding[3];
} __attribute__((packed)) v3_event_t;

bool v3_events_post(resource_id_t id, v3_event_type_t type);
size_t v3_events_poll(v3_event_t *out_batch, size_t max_count);
uint32_t v3_events_get_drop_count(void);

#endif // V3_EVENTS_H

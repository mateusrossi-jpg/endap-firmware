#ifndef V3_CATALOG_H
#define V3_CATALOG_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "v3_identity.h"

#define V3_CATALOG_MAX_RESOURCES 1024

typedef enum {
    RESOURCE_STATE_UNKNOWN = 0,
    RESOURCE_STATE_ONLINE  = 1,
    RESOURCE_STATE_OFFLINE = 2,
    RESOURCE_STATE_LOST    = 3
} resource_status_t;

/* Registro Canônico de Instância (Tamanho estrito: 32 Bytes) */
typedef struct {
    resource_id_t     id;             // 8 Bytes (0..7)
    resource_key_t    key;            // 8 Bytes (8..15)
    uint32_t          last_seen_tick; // 4 Bytes (16..19)
    uint16_t          status;         // 2 Bytes (20..21)
    uint16_t          sync_flags;     // 2 Bytes (22..23)
    uint32_t          desired_state;  // 4 Bytes (24..27)
    uint32_t          reported_state; // 4 Bytes (28..31)
} __attribute__((packed)) resource_record_t;

typedef struct resource_backend_interface {
    bool (*insert)(struct resource_backend_interface *self, const resource_record_t *record);
    const resource_record_t* (*find)(struct resource_backend_interface *self, resource_id_t id);
    const resource_record_t* (*get_by_index)(struct resource_backend_interface *self, size_t index);
    bool (*update_desired)(struct resource_backend_interface *self, resource_id_t id, uint32_t desired);
    bool (*update_reported)(struct resource_backend_interface *self, resource_id_t id, uint32_t reported);
    bool (*remove)(struct resource_backend_interface *self, resource_id_t id);
    size_t (*size)(struct resource_backend_interface *self);
    bool (*snapshot)(struct resource_backend_interface *self, resource_record_t *out_array, size_t *in_out_count);
} resource_backend_t;

resource_backend_t* v3_catalog_get_default_backend(void);

#endif // V3_CATALOG_H

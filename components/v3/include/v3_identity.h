#ifndef V3_IDENTITY_H
#define V3_IDENTITY_H

#include <stdint.h>

typedef uint64_t resource_id_t;

typedef union {
    uint64_t raw;
    struct {
        uint32_t gateway_id; // Logical Slot
        uint16_t node_uid;
        uint8_t  io_id;
        uint8_t  reserved_flags; // Fecha os 8 bytes exatos
    } __attribute__((packed));
} resource_key_t;

#endif // V3_IDENTITY_H

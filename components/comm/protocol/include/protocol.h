#ifndef PROTOCOL_H
#define PROTOCOL_H

#include <stdint.h>

/*
============================================================
 Protocol Interface
============================================================
*/

void protocol_init(void);
void protocol_process_frame(const uint8_t *data, uint16_t len);

/*
============================================================
 Protocol Messages
============================================================
*/

#define PROTOCOL_MSG_CLUSTER_IO_UPDATE   0x10
#define PROTOCOL_MSG_OUTPUT_COMMAND      0x11

typedef struct {
    uint16_t io_id;
    uint32_t new_owner;
    uint32_t original_owner;
} protocol_cluster_io_update_t;

typedef struct {
    uint32_t target_node;
    uint32_t requester_node;
    uint16_t output_id;
    int32_t value;
} protocol_output_command_t;

typedef void (*protocol_io_update_cb_t)(const protocol_cluster_io_update_t *msg);
typedef void (*protocol_output_command_cb_t)(const protocol_output_command_t *msg);

void protocol_register_io_update_callback(protocol_io_update_cb_t cb);
void protocol_register_output_command_callback(protocol_output_command_cb_t cb);

typedef struct {
    uint8_t type;

    union {
        protocol_cluster_io_update_t io_update;
        protocol_output_command_t output_command;
    } data;

} protocol_msg_t;

#endif

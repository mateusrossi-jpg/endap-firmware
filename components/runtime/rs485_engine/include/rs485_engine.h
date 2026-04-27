#ifndef RS485_ENGINE_H
#define RS485_ENGINE_H

#include <stdint.h>
#include <stdbool.h>

/* ============================================================
   CONFIG
============================================================ */

#define RS485_ENGINE_MAX_NODES   16
#define RS485_ENGINE_TIMEOUT_MS  100
#define RS485_ENGINE_MAX_RETRY   3

#define RS485_FRAME_MAX_PAYLOAD  64
#define RS485_ENGINE_SELF_TEST_DEFAULT      0
#define RS485_ENGINE_SELF_TEST_NODE_ID      1
#define RS485_ENGINE_SELF_TEST_ACK_DELAY_US 500

typedef enum
{
    RS485_FRAME_TYPE_POLL = 1,
    RS485_FRAME_TYPE_ACK  = 2,
    RS485_FRAME_TYPE_CLUSTER_HEARTBEAT = 0x10,
    RS485_FRAME_TYPE_CLUSTER_FRAME = 0x11
} rs485_frame_type_t;

typedef enum
{
    RS485_ENGINE_RX_NONE = 0,
    RS485_ENGINE_RX_FRAME,
    RS485_ENGINE_RX_CRC_ERROR,
    RS485_ENGINE_RX_FORMAT_ERROR
} rs485_engine_rx_result_t;

/* ============================================================
   NODE STATE
============================================================ */

typedef enum
{
    NODE_STATE_UNKNOWN = 0,
    NODE_STATE_ONLINE,
    NODE_STATE_OFFLINE

} node_state_t;


/* ============================================================
   RS485 FRAME
============================================================ */

typedef struct
{
    uint8_t  node;
    uint16_t msg_id;
    uint8_t  type;
    uint8_t  len;
    uint8_t  payload[RS485_FRAME_MAX_PAYLOAD];

} rs485_frame_t;

typedef struct
{
    bool enabled;
    bool self_test_enabled;
    bool self_test_active;
    uint32_t tx_count;
    uint32_t tx_bytes;
    uint32_t rx_count;
    uint32_t rx_bytes;
    uint32_t crc_error_count;
    uint32_t format_error_count;
    uint32_t last_tx_ms;
    uint32_t last_rx_ms;
} rs485_engine_metrics_t;


/* ============================================================
   ENGINE CONTROL
============================================================ */

void rs485_engine_init(void);

void rs485_engine_enable(void);

void rs485_engine_disable(void);

void rs485_engine_set_self_test(bool enabled);
bool rs485_engine_self_test_enabled(void);

void rs485_engine_tick_1ms(void);

void rs485_engine_on_ack(uint8_t node_id, uint16_t msg_id);


/* ============================================================
   RECEIVE FRAME
============================================================ */

rs485_engine_rx_result_t rs485_engine_receive(rs485_frame_t *frame);


/* ============================================================
   SEND FRAME
============================================================ */

void rs485_engine_send(rs485_frame_t *frame);
void rs485_engine_get_metrics(rs485_engine_metrics_t *out_metrics);


/* ============================================================
   NODE STATUS
============================================================ */

uint8_t rs485_engine_node_online(uint8_t node_id);

node_state_t rs485_engine_node_state(uint8_t node_id);


typedef void (*rs485_engine_external_frame_cb_t)(const rs485_frame_t *frame);
void rs485_engine_register_external_frame_callback(rs485_engine_external_frame_cb_t cb);


#endif

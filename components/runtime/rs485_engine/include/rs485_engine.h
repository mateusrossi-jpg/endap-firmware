#ifndef RS485_ENGINE_H
#define RS485_ENGINE_H

#include <stdint.h>

/* ============================================================
   CONFIG
============================================================ */

#define RS485_ENGINE_MAX_NODES   16
#define RS485_ENGINE_TIMEOUT_MS  3
#define RS485_ENGINE_MAX_RETRY   3

#define RS485_FRAME_MAX_PAYLOAD  64


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


/* ============================================================
   ENGINE CONTROL
============================================================ */

void rs485_engine_init(void);

void rs485_engine_enable(void);

void rs485_engine_disable(void);

void rs485_engine_tick_1ms(void);

void rs485_engine_on_ack(uint8_t node_id, uint16_t msg_id);


/* ============================================================
   SEND FRAME
============================================================ */

void rs485_engine_send(rs485_frame_t *frame);


/* ============================================================
   NODE STATUS
============================================================ */

uint8_t rs485_engine_node_online(uint8_t node_id);

node_state_t rs485_engine_node_state(uint8_t node_id);


#endif

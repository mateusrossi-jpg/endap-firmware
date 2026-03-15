#include "rs485_engine.h"

#include "esp_timer.h"
#include <string.h>

/* ============================================================
NODE STRUCTURE
============================================================ */

typedef struct
{
    node_state_t state;
    uint64_t last_seen_us;
    uint8_t retry;

} node_entry_t;

/* ============================================================
NODE TABLE
============================================================ */

static node_entry_t nodes[RS485_ENGINE_MAX_NODES];

static uint8_t engine_enabled = 0;

/* ============================================================
INIT
============================================================ */

void rs485_engine_init(void)
{
    memset(nodes,0,sizeof(nodes));

    for(int i=0;i<RS485_ENGINE_MAX_NODES;i++)
        nodes[i].state = NODE_STATE_UNKNOWN;
}

/* ============================================================
ENABLE / DISABLE
============================================================ */

void rs485_engine_enable(void)
{
    engine_enabled = 1;
}

void rs485_engine_disable(void)
{
    engine_enabled = 0;
}

/* ============================================================
NODE STATE
============================================================ */

uint8_t rs485_engine_node_online(uint8_t node_id)
{
    if(node_id >= RS485_ENGINE_MAX_NODES)
        return 0;

    return nodes[node_id].state == NODE_STATE_ONLINE;
}

node_state_t rs485_engine_node_state(uint8_t node_id)
{
    if(node_id >= RS485_ENGINE_MAX_NODES)
        return NODE_STATE_UNKNOWN;

    return nodes[node_id].state;
}

/* ============================================================
ACK RECEIVED
============================================================ */

void rs485_engine_on_ack(uint8_t node_id, uint16_t msg_id)
{
    if(node_id >= RS485_ENGINE_MAX_NODES)
        return;

    nodes[node_id].state = NODE_STATE_ONLINE;
    nodes[node_id].last_seen_us = esp_timer_get_time();
    nodes[node_id].retry = 0;
}

/* ============================================================
ENGINE TICK
============================================================ */

void rs485_engine_tick_1ms(void)
{
    if(!engine_enabled)
        return;

    uint64_t now = esp_timer_get_time();

    for(int i=0;i<RS485_ENGINE_MAX_NODES;i++)
    {
        if(nodes[i].state != NODE_STATE_ONLINE)
            continue;

        uint64_t diff = now - nodes[i].last_seen_us;

        if(diff > (RS485_ENGINE_TIMEOUT_MS * 1000))
            nodes[i].state = NODE_STATE_OFFLINE;
    }
}

/* ============================================================
SEND FRAME
============================================================ */

void rs485_engine_send(rs485_frame_t *frame)
{
    if(!engine_enabled)
        return;

    /* envio real ocorre no driver */
}

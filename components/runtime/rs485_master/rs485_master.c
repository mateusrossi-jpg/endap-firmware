#include "rs485_master.h"
//#include "rs485_protocol.h"
#include "rs485_engine.h"

#include "esp_timer.h"

#include <string.h>

#define RESPONSE_TIMEOUT_US 3000

/* ============================================================
NODE STRUCTURE
============================================================ */

typedef struct
{
    uint8_t id;
    uint8_t online;
    uint8_t retry_count;
    uint32_t last_seen_ms;

} node_status_t;

/* ============================================================
NODE TABLE
============================================================ */

static node_status_t nodes[RS485_MAX_NODES];

static uint8_t current_node = 0;

static uint8_t waiting_response = 0;

static uint16_t current_msg_id = 0;

static uint16_t msg_id_counter = 1;

static int64_t timeout_start = 0;

/* ============================================================
INIT
============================================================ */

void rs485_master_init(void)
{
    memset(nodes,0,sizeof(nodes));

    rs485_engine_init();
    rs485_engine_enable();

    for(int i=0;i<RS485_MAX_NODES;i++)
        nodes[i].id = i+1;

   // rs485_protocol_register_ack_callback(rs485_master_on_ack);
}

/* ============================================================
ACK HANDLER
============================================================ */

void rs485_master_on_ack(uint8_t node_id,uint16_t msg_id)
{
    if(!waiting_response)
        return;

    if(msg_id != current_msg_id)
        return;

    waiting_response = 0;

    rs485_engine_on_ack(node_id,msg_id);

    uint8_t idx = node_id-1;

    nodes[idx].online = 1;
    nodes[idx].retry_count = 0;

    nodes[idx].last_seen_ms =
        esp_timer_get_time()/1000;
}

/* ============================================================
MASTER TICK
============================================================ */

void rs485_master_tick(void)
{
    int64_t now = esp_timer_get_time();

    node_status_t *node = &nodes[current_node];

    if(waiting_response)
    {
        if((now - timeout_start) > RESPONSE_TIMEOUT_US)
        {
            node->retry_count++;

            if(node->retry_count >= RS485_MAX_RETRY)
            {
                node->online = 0;
                node->retry_count = 0;
            }

            waiting_response = 0;

            current_node++;

            if(current_node >= RS485_MAX_NODES)
                current_node = 0;
        }

        return;
    }

    uint16_t msg_id = msg_id_counter++;

    current_msg_id = msg_id;

    waiting_response = 1;

    timeout_start = now;

  // rs485_protocol_send(
  //     node->id,
  //   msg_id,
  //     FRAME_TYPE_POLL,
  //    NULL,
  //     0
  //  );
}

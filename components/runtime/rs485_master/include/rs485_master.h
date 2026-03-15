#pragma once

#include <stdint.h>

#define RS485_MAX_NODES 16
#define RS485_MAX_RETRY 3

void rs485_master_init(void);

void rs485_master_tick(void);

void rs485_master_on_ack(uint8_t node_id, uint16_t msg_id);

uint8_t rs485_master_node_online(uint8_t node_id);

uint32_t rs485_master_node_last_seen(uint8_t node_id);

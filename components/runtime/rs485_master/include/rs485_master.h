#pragma once

#ifndef RS485_MASTER_H
#define RS485_MASTER_H

#include <stdint.h>
#include <stdbool.h>

#define RS485_MAX_NODES 16
#define RS485_MAX_RETRY 3

/* INIT */
void rs485_master_init(void);

typedef enum
{
    RS485_MASTER_EVENT_NONE = 0,
    RS485_MASTER_EVENT_TX,
    RS485_MASTER_EVENT_ACK,
    RS485_MASTER_EVENT_RETRY,
    RS485_MASTER_EVENT_TIMEOUT,
    RS485_MASTER_EVENT_CRC_ERROR,
    RS485_MASTER_EVENT_RX_IGNORED
} rs485_master_event_type_t;

typedef struct
{
    rs485_master_event_type_t type;
    uint8_t node_id;
    uint16_t msg_id;
    uint32_t latency_us;
} rs485_master_event_t;

typedef struct
{
    bool enabled;
    bool waiting_response;
    bool retry_pending;
    bool self_test_enabled;
    uint8_t awaiting_node;
    uint8_t online_nodes;
    uint32_t tx_count;
    uint32_t ack_count;
    uint32_t retry_count;
    uint32_t timeout_count;
    uint32_t crc_error_count;
    uint32_t rx_ignored_count;
    uint32_t last_tx_ms;
    uint32_t last_ack_ms;
    uint32_t last_timeout_ms;
} rs485_master_metrics_t;

typedef struct
{
    uint32_t samples;

    uint32_t rx_avg_us;
    uint32_t rx_max_us;

    uint32_t frame_events;
    uint32_t frame_avg_us;
    uint32_t frame_max_us;

    uint32_t timeout_events;
    uint32_t timeout_avg_us;
    uint32_t timeout_max_us;

    uint32_t tx_events;
    uint32_t tx_avg_us;
    uint32_t tx_max_us;

    uint32_t total_avg_us;
    uint32_t total_max_us;

    uint32_t ack_events;
    uint32_t ignored_events;
    uint32_t retry_events;
    uint32_t crc_events;
} rs485_master_profile_metrics_t;

/* ACK */
bool rs485_master_on_ack(uint8_t node_id, uint16_t msg_id, uint32_t *latency_us);

uint8_t rs485_master_node_online(uint8_t node_id);
uint32_t rs485_master_node_last_seen(uint8_t node_id);
uint8_t rs485_master_online_count(void);
void rs485_master_get_metrics(rs485_master_metrics_t *out_metrics);

/* deterministic scheduler */
bool rs485_master_has_work(void);
bool rs485_master_process_one(rs485_master_event_t *event);

/* debug profiling */
void rs485_master_profile_snapshot(rs485_master_profile_metrics_t *out, bool reset);

#endif

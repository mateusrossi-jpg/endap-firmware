#include "rs485_master.h"
#include "rs485_engine.h"

#include "esp_timer.h"
#include "esp_attr.h"

#include <stdbool.h>
#include <string.h>

#define RESPONSE_TIMEOUT_US      6000ULL
#define RETRY_BACKOFF_US         2000ULL
#define IDLE_POLL_INTERVAL_US    4000ULL
#define SELF_TEST_POLL_US        1000ULL

typedef struct
{
    uint8_t id;
    uint8_t online;
    uint8_t retry_count;
    uint32_t last_seen_ms;
} node_status_t;

typedef struct
{
    uint32_t samples;

    uint32_t rx_sum_us;
    uint32_t rx_max_us;

    uint32_t frame_events;
    uint32_t frame_sum_us;
    uint32_t frame_max_us;

    uint32_t timeout_events;
    uint32_t timeout_sum_us;
    uint32_t timeout_max_us;

    uint32_t tx_events;
    uint32_t tx_sum_us;
    uint32_t tx_max_us;

    uint32_t total_sum_us;
    uint32_t total_max_us;

    uint32_t ack_events;
    uint32_t ignored_events;
    uint32_t retry_events;
    uint32_t crc_events;
} rs485_master_profile_acc_t;

static node_status_t nodes[RS485_MAX_NODES];

static uint8_t current_node = 0;
static uint8_t awaiting_node = 0;
static bool waiting_response = false;
static bool retry_pending = false;
static rs485_master_metrics_t metrics;
static rs485_master_profile_acc_t profile;

static uint16_t current_msg_id = 0;
static uint16_t msg_id_counter = 1;

static uint64_t request_start_us = 0;
static uint64_t next_poll_due_us = 0;

static inline uint32_t profile_delta_us(uint64_t start, uint64_t end)
{
    return (uint32_t)((end >= start) ? (end - start) : 0U);
}

static inline void profile_accumulate(uint32_t *sum, uint32_t *max, uint32_t dt)
{
    *sum += dt;

    if (dt > *max)
        *max = dt;
}

static inline void rs485_master_profile_finish(uint64_t total_start)
{
    uint32_t total_dt = profile_delta_us(total_start, (uint64_t)esp_timer_get_time());

    profile.samples++;
    profile_accumulate(&profile.total_sum_us, &profile.total_max_us, total_dt);
}

static uint16_t next_msg_id(void)
{
    uint16_t msg_id = msg_id_counter++;

    if (msg_id_counter == 0)
        msg_id_counter = 1;

    return msg_id;
}

static uint64_t rs485_master_now_us(void)
{
    return (uint64_t)esp_timer_get_time();
}

static uint64_t rs485_master_idle_interval_us(void)
{
    return rs485_engine_self_test_enabled() ? SELF_TEST_POLL_US : IDLE_POLL_INTERVAL_US;
}

static void rs485_master_schedule_next_poll(uint64_t now_us, uint64_t delay_us)
{
    next_poll_due_us = now_us + delay_us;
}

/* ========================================================= */

void rs485_master_init(void)
{
    memset(nodes, 0, sizeof(nodes));
    memset(&metrics, 0, sizeof(metrics));
    memset(&profile, 0, sizeof(profile));

    current_node = 0;
    awaiting_node = 0;
    waiting_response = false;
    retry_pending = false;
    current_msg_id = 0;
    msg_id_counter = 1;
    request_start_us = 0;
    next_poll_due_us = 0;

    rs485_engine_init();
    rs485_engine_enable();
    metrics.enabled = true;
    metrics.self_test_enabled = rs485_engine_self_test_enabled();

    for (int i = 0; i < RS485_MAX_NODES; i++)
        nodes[i].id = (uint8_t)(i + 1);

    rs485_master_schedule_next_poll(rs485_master_now_us(), rs485_master_idle_interval_us());
}

/* ========================================================= */

bool rs485_master_on_ack(uint8_t node_id, uint16_t msg_id, uint32_t *latency_us)
{
    if (!waiting_response)
        return false;

    if (node_id != awaiting_node)
        return false;

    if (msg_id != current_msg_id)
        return false;

    uint8_t idx = (uint8_t)(node_id - 1U);

    if (idx >= RS485_MAX_NODES)
        return false;

    uint64_t now = rs485_master_now_us();

    if (latency_us)
        *latency_us = (uint32_t)((now >= request_start_us) ? (now - request_start_us) : 0U);

    waiting_response = false;
    retry_pending = false;
    awaiting_node = 0;
    request_start_us = 0;

    rs485_engine_on_ack(node_id, msg_id);

    nodes[idx].online = 1U;
    nodes[idx].retry_count = 0U;
    nodes[idx].last_seen_ms = (uint32_t)(now / 1000ULL);
    metrics.ack_count++;
    metrics.last_ack_ms = nodes[idx].last_seen_ms;

    rs485_master_schedule_next_poll(now, rs485_master_idle_interval_us());
    return true;
}

uint8_t rs485_master_node_online(uint8_t node_id)
{
    uint8_t idx = (uint8_t)(node_id - 1U);

    if (idx >= RS485_MAX_NODES)
        return 0;

    return nodes[idx].online;
}

uint32_t rs485_master_node_last_seen(uint8_t node_id)
{
    uint8_t idx = (uint8_t)(node_id - 1U);

    if (idx >= RS485_MAX_NODES)
        return 0;

    return nodes[idx].last_seen_ms;
}

uint8_t rs485_master_online_count(void)
{
    uint8_t count = 0;

    for (int i = 0; i < RS485_MAX_NODES; i++)
    {
        if (nodes[i].online)
            count++;
    }

    return count;
}

void rs485_master_get_metrics(rs485_master_metrics_t *out_metrics)
{
    if (!out_metrics)
        return;

    metrics.enabled = true;
    metrics.waiting_response = waiting_response;
    metrics.retry_pending = retry_pending;
    metrics.awaiting_node = awaiting_node;
    metrics.online_nodes = rs485_master_online_count();
    metrics.self_test_enabled = rs485_engine_self_test_enabled();
    *out_metrics = metrics;
}

void rs485_master_profile_snapshot(rs485_master_profile_metrics_t *out, bool reset)
{
    if (!out)
        return;

    memset(out, 0, sizeof(*out));

    out->samples = profile.samples;

    out->rx_max_us = profile.rx_max_us;
    out->frame_events = profile.frame_events;
    out->frame_max_us = profile.frame_max_us;
    out->timeout_events = profile.timeout_events;
    out->timeout_max_us = profile.timeout_max_us;
    out->tx_events = profile.tx_events;
    out->tx_max_us = profile.tx_max_us;
    out->total_max_us = profile.total_max_us;
    out->ack_events = profile.ack_events;
    out->ignored_events = profile.ignored_events;
    out->retry_events = profile.retry_events;
    out->crc_events = profile.crc_events;

    if (profile.samples)
    {
        out->rx_avg_us = profile.rx_sum_us / profile.samples;
        out->total_avg_us = profile.total_sum_us / profile.samples;
    }

    if (profile.frame_events)
        out->frame_avg_us = profile.frame_sum_us / profile.frame_events;

    if (profile.timeout_events)
        out->timeout_avg_us = profile.timeout_sum_us / profile.timeout_events;

    if (profile.tx_events)
        out->tx_avg_us = profile.tx_sum_us / profile.tx_events;

    if (reset)
        memset(&profile, 0, sizeof(profile));
}

/* ========================================================= */
/* deterministic scheduler                                   */
/* ========================================================= */

bool IRAM_ATTR rs485_master_has_work(void)
{
    uint64_t now = rs485_master_now_us();

    if (waiting_response)
        return true;

    if (retry_pending)
        return true;

    return now >= next_poll_due_us;
}

/* ========================================================= */

bool IRAM_ATTR rs485_master_process_one(rs485_master_event_t *event)
{
    uint64_t total_start = rs485_master_now_us();
    uint64_t t0;
    uint64_t t1;

    uint64_t now;
    rs485_frame_t rx_frame = {0};
    rs485_engine_rx_result_t rx_result;

    if (event)
        memset(event, 0, sizeof(*event));

    t0 = rs485_master_now_us();
    rx_result = rs485_engine_receive(&rx_frame);
    t1 = rs485_master_now_us();
    profile_accumulate(&profile.rx_sum_us, &profile.rx_max_us, profile_delta_us(t0, t1));

    if (rx_result == RS485_ENGINE_RX_CRC_ERROR)
    {
        metrics.crc_error_count++;
        profile.crc_events++;

        if (event)
            event->type = RS485_MASTER_EVENT_CRC_ERROR;

        rs485_master_profile_finish(total_start);
        return true;
    }

    if (rx_result == RS485_ENGINE_RX_FRAME)
    {
        t0 = rs485_master_now_us();

        if ((rx_frame.type == RS485_FRAME_TYPE_ACK) && (rx_frame.len == 0U))
        {
            uint32_t latency_us = 0U;

            if (rs485_master_on_ack(rx_frame.node, rx_frame.msg_id, &latency_us))
            {
                profile.ack_events++;

                if (event)
                {
                    event->type = RS485_MASTER_EVENT_ACK;
                    event->node_id = rx_frame.node;
                    event->msg_id = rx_frame.msg_id;
                    event->latency_us = latency_us;
                }
            }
            else
            {
                metrics.rx_ignored_count++;
                profile.ignored_events++;

                if (event)
                {
                    event->type = RS485_MASTER_EVENT_RX_IGNORED;
                    event->node_id = rx_frame.node;
                    event->msg_id = rx_frame.msg_id;
                }
            }
        }
        else
        {
            metrics.rx_ignored_count++;
            profile.ignored_events++;

            if (event)
            {
                event->type = RS485_MASTER_EVENT_RX_IGNORED;
                event->node_id = rx_frame.node;
                event->msg_id = rx_frame.msg_id;
            }
        }

        t1 = rs485_master_now_us();
        profile.frame_events++;
        profile_accumulate(&profile.frame_sum_us, &profile.frame_max_us, profile_delta_us(t0, t1));

        rs485_master_profile_finish(total_start);
        return true;
    }

    now = rs485_master_now_us();

    /* timeout check */
    if (waiting_response)
    {
        if ((now - request_start_us) <= RESPONSE_TIMEOUT_US)
        {
            rs485_master_profile_finish(total_start);
            return false;
        }

        t0 = rs485_master_now_us();

        if (awaiting_node == 0U || awaiting_node > RS485_MAX_NODES)
        {
            waiting_response = false;
            retry_pending = false;
            current_msg_id = 0U;
            request_start_us = 0U;
            rs485_master_schedule_next_poll(now, rs485_master_idle_interval_us());

            t1 = rs485_master_now_us();
            profile.timeout_events++;
            profile_accumulate(&profile.timeout_sum_us, &profile.timeout_max_us, profile_delta_us(t0, t1));

            rs485_master_profile_finish(total_start);
            return false;
        }

        node_status_t *node = &nodes[awaiting_node - 1U];
        node->retry_count++;

        waiting_response = false;
        request_start_us = 0U;

        if (node->retry_count < RS485_MAX_RETRY)
        {
            retry_pending = true;
            metrics.retry_count++;
            metrics.last_timeout_ms = (uint32_t)(now / 1000ULL);
            profile.retry_events++;
            rs485_master_schedule_next_poll(now, RETRY_BACKOFF_US);

            if (event)
            {
                event->type = RS485_MASTER_EVENT_RETRY;
                event->node_id = awaiting_node;
                event->msg_id = current_msg_id;
            }

            t1 = rs485_master_now_us();
            profile.timeout_events++;
            profile_accumulate(&profile.timeout_sum_us, &profile.timeout_max_us, profile_delta_us(t0, t1));

            rs485_master_profile_finish(total_start);
            return true;
        }

        node->online = 0U;
        node->retry_count = 0U;
        metrics.timeout_count++;
        metrics.last_timeout_ms = (uint32_t)(now / 1000ULL);
        profile.retry_events++;

        if (event)
        {
            event->type = RS485_MASTER_EVENT_TIMEOUT;
            event->node_id = awaiting_node;
            event->msg_id = current_msg_id;
        }

        awaiting_node = 0U;
        retry_pending = false;
        current_msg_id = 0U;
        rs485_master_schedule_next_poll(now, rs485_master_idle_interval_us());

        t1 = rs485_master_now_us();
        profile.timeout_events++;
        profile_accumulate(&profile.timeout_sum_us, &profile.timeout_max_us, profile_delta_us(t0, t1));

        rs485_master_profile_finish(total_start);
        return true;
    }

    if (now < next_poll_due_us)
    {
        rs485_master_profile_finish(total_start);
        return false;
    }

    /* send a single operation */
    t0 = rs485_master_now_us();

    uint8_t node_id;
    uint16_t msg_id;

    if (retry_pending)
    {
        node_id = awaiting_node;
        msg_id = current_msg_id;
        retry_pending = false;
    }
    else
    {
        if (rs485_engine_self_test_enabled())
        {
            node_id = RS485_ENGINE_SELF_TEST_NODE_ID;
        }
        else
        {
            node_id = (uint8_t)(current_node + 1U);
        }

        msg_id = next_msg_id();
        current_msg_id = msg_id;

        if (!rs485_engine_self_test_enabled())
        {
            current_node++;
            if (current_node >= RS485_MAX_NODES)
                current_node = 0U;
        }
    }

    waiting_response = true;
    awaiting_node = node_id;
    request_start_us = now;

    rs485_frame_t frame = {
        .node = node_id,
        .msg_id = msg_id,
        .type = RS485_FRAME_TYPE_POLL,
        .len = 0U
    };

    rs485_engine_send(&frame);
    metrics.tx_count++;
    metrics.last_tx_ms = (uint32_t)(now / 1000ULL);
    rs485_master_schedule_next_poll(now, RESPONSE_TIMEOUT_US);

    if (event)
    {
        event->type = RS485_MASTER_EVENT_TX;
        event->node_id = node_id;
        event->msg_id = msg_id;
    }

    t1 = rs485_master_now_us();
    profile.tx_events++;
    profile_accumulate(&profile.tx_sum_us, &profile.tx_max_us, profile_delta_us(t0, t1));

    rs485_master_profile_finish(total_start);
    return true;
}

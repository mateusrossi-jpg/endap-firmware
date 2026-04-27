#include "rs485_engine.h"
#include "parser.h"
#include "rs485.h"
#include "node_identity.h"

#include "esp_log.h"
#include "esp_timer.h"
#include "esp_attr.h"

#include <ctype.h>
#include <string.h>

#define RS485_FRAME_HEADER_SIZE   5
#define RS485_FRAME_CRC_SIZE      2
#define RS485_FRAME_RAW_MAX       (RS485_FRAME_HEADER_SIZE + RS485_FRAME_MAX_PAYLOAD + RS485_FRAME_CRC_SIZE)
#define RS485_FRAME_WIRE_MAX      ((RS485_FRAME_RAW_MAX * 2) + 2)
#define RS485_ENGINE_RX_BYTES_PER_CALL 16
#define TAG "RS485_ENGINE"

typedef struct
{
    node_state_t state;
    uint64_t last_seen_us;
    uint8_t retry;
} node_entry_t;

static node_entry_t nodes[RS485_ENGINE_MAX_NODES];
static rs485_engine_metrics_t metrics;

static uint8_t engine_enabled = 0;
static bool self_test_enabled = RS485_ENGINE_SELF_TEST_DEFAULT;
static bool self_test_pending = false;
static uint8_t self_test_wire[RS485_FRAME_WIRE_MAX];
static uint16_t self_test_wire_len = 0;
static uint64_t self_test_ready_us = 0;
static uint8_t local_node_id = 0;
static rs485_engine_external_frame_cb_t external_frame_cb = NULL;

static uint16_t rs485_crc16(const uint8_t *data, uint16_t len)
{
    uint16_t crc = 0xFFFF;

    for (uint16_t i = 0; i < len; i++)
    {
        crc ^= data[i];

        for (uint8_t bit = 0; bit < 8; bit++)
        {
            if (crc & 1U)
                crc = (uint16_t)((crc >> 1U) ^ 0xA001U);
            else
                crc >>= 1U;
        }
    }

    return crc;
}

static char nibble_to_hex(uint8_t value)
{
    value &= 0x0F;
    return (value < 10U) ? (char)('0' + value) : (char)('A' + (value - 10U));
}

static int hex_to_nibble(uint8_t value)
{
    if (value >= '0' && value <= '9')
        return value - '0';

    value = (uint8_t)toupper(value);

    if (value >= 'A' && value <= 'F')
        return 10 + (value - 'A');

    return -1;
}

static uint16_t encode_raw_frame(const rs485_frame_t *frame, uint8_t *raw, uint16_t max_len)
{
    if (!frame || !raw)
        return 0U;

    if (frame->len > RS485_FRAME_MAX_PAYLOAD)
        return 0U;

    uint16_t raw_len = (uint16_t)(RS485_FRAME_HEADER_SIZE + frame->len + RS485_FRAME_CRC_SIZE);

    if (raw_len > max_len)
        return 0U;

    raw[0] = frame->node;
    raw[1] = (uint8_t)(frame->msg_id >> 8);
    raw[2] = (uint8_t)(frame->msg_id & 0xFF);
    raw[3] = frame->type;
    raw[4] = frame->len;

    if (frame->len)
        memcpy(&raw[5], frame->payload, frame->len);

    uint16_t crc = rs485_crc16(raw, (uint16_t)(RS485_FRAME_HEADER_SIZE + frame->len));
    raw[5 + frame->len] = (uint8_t)(crc >> 8);
    raw[6 + frame->len] = (uint8_t)(crc & 0xFF);

    return raw_len;
}

static uint16_t encode_wire_frame(const rs485_frame_t *frame, uint8_t *wire, uint16_t max_len)
{
    uint8_t raw[RS485_FRAME_RAW_MAX];
    uint16_t raw_len = encode_raw_frame(frame, raw, sizeof(raw));

    if (raw_len == 0U)
        return 0U;

    uint16_t wire_len = (uint16_t)((raw_len * 2U) + 2U);

    if (wire_len > max_len)
        return 0U;

    wire[0] = '<';

    for (uint16_t i = 0; i < raw_len; i++)
    {
        wire[1U + (i * 2U)] = (uint8_t)nibble_to_hex((uint8_t)(raw[i] >> 4));
        wire[2U + (i * 2U)] = (uint8_t)nibble_to_hex(raw[i]);
    }

    wire[wire_len - 1U] = '>';

    return wire_len;
}

static rs485_engine_rx_result_t decode_wire_frame(const uint8_t *wire, uint16_t wire_len, rs485_frame_t *frame)
{
    uint8_t raw[RS485_FRAME_RAW_MAX];

    if (!wire || !frame)
        return RS485_ENGINE_RX_FORMAT_ERROR;

    if ((wire_len == 0U) || ((wire_len & 1U) != 0U))
        return RS485_ENGINE_RX_FORMAT_ERROR;

    uint16_t raw_len = (uint16_t)(wire_len / 2U);

    if ((raw_len < (RS485_FRAME_HEADER_SIZE + RS485_FRAME_CRC_SIZE)) || (raw_len > sizeof(raw)))
        return RS485_ENGINE_RX_FORMAT_ERROR;

    for (uint16_t i = 0; i < raw_len; i++)
    {
        int hi = hex_to_nibble(wire[i * 2U]);
        int lo = hex_to_nibble(wire[(i * 2U) + 1U]);

        if ((hi < 0) || (lo < 0))
            return RS485_ENGINE_RX_FORMAT_ERROR;

        raw[i] = (uint8_t)((hi << 4) | lo);
    }

    uint8_t payload_len = raw[4];
    uint16_t expected_len = (uint16_t)(RS485_FRAME_HEADER_SIZE + payload_len + RS485_FRAME_CRC_SIZE);

    if (expected_len != raw_len)
        return RS485_ENGINE_RX_FORMAT_ERROR;

    uint16_t expected_crc = (uint16_t)((raw[expected_len - 2U] << 8) | raw[expected_len - 1U]);
    uint16_t actual_crc = rs485_crc16(raw, (uint16_t)(expected_len - RS485_FRAME_CRC_SIZE));

    if (expected_crc != actual_crc)
        return RS485_ENGINE_RX_CRC_ERROR;

    frame->node = raw[0];
    frame->msg_id = (uint16_t)((raw[1] << 8) | raw[2]);
    frame->type = raw[3];
    frame->len = payload_len;

    if (payload_len)
        memcpy(frame->payload, &raw[5], payload_len);

    return RS485_ENGINE_RX_FRAME;
}

static uint8_t rs485_engine_compute_local_node_id(void)
{
    uint32_t uid = node_identity_get();
    uint8_t node_id = (uint8_t)((uid % RS485_ENGINE_MAX_NODES) + 1U);

    if (node_id == 0U)
        node_id = 1U;

    return node_id;
}

static uint8_t rs485_engine_local_node_id(void)
{
    if (local_node_id == 0U)
    {
        node_identity_init();
        local_node_id = rs485_engine_compute_local_node_id();
        ESP_LOGI(TAG, "Node local RS485=%u", (unsigned)local_node_id);
    }

    return local_node_id;
}

static bool rs485_engine_should_reply_to_poll(const rs485_frame_t *frame)
{
    if (!frame)
        return false;

    if (frame->type != RS485_FRAME_TYPE_POLL)
        return false;

    if (frame->len != 0U)
        return false;

    return frame->node == rs485_engine_local_node_id();
}

static void rs485_engine_reply_ack(const rs485_frame_t *poll)
{
    rs485_frame_t ack = {
        .node = poll->node,
        .msg_id = poll->msg_id,
        .type = RS485_FRAME_TYPE_ACK,
        .len = 0U
    };

    rs485_engine_send(&ack);
}

static void rs485_engine_feed_parser_bounded(const uint8_t *bytes, uint16_t len)
{
    uint16_t budget = len;

    if (budget > RS485_ENGINE_RX_BYTES_PER_CALL)
        budget = RS485_ENGINE_RX_BYTES_PER_CALL;

    for (uint16_t i = 0; i < budget; i++)
    {
        if (parser_process_byte(bytes[i]))
            break;
    }
}

void rs485_engine_init(void)
{
    memset(nodes, 0, sizeof(nodes));
    memset(&metrics, 0, sizeof(metrics));
    parser_init();
    node_identity_init();
    local_node_id = rs485_engine_compute_local_node_id();
    self_test_pending = false;
    self_test_wire_len = 0U;
    self_test_ready_us = 0U;
    metrics.self_test_enabled = self_test_enabled;
    metrics.self_test_active = false;

    for (int i = 0; i < RS485_ENGINE_MAX_NODES; i++)
        nodes[i].state = NODE_STATE_UNKNOWN;

    ESP_LOGI(TAG, "Engine iniciado (local_node=%u)", (unsigned)local_node_id);

    if (self_test_enabled)
    {
        ESP_LOGW(TAG,
                 "Self-test mode ativo para node %d (ack em %d us)",
                 RS485_ENGINE_SELF_TEST_NODE_ID,
                 RS485_ENGINE_SELF_TEST_ACK_DELAY_US);
    }
}

void rs485_engine_enable(void)
{
    engine_enabled = 1U;
    metrics.enabled = true;
}

void rs485_engine_disable(void)
{
    engine_enabled = 0U;
    metrics.enabled = false;
    metrics.self_test_active = false;
}

void rs485_engine_set_self_test(bool enabled)
{
    self_test_enabled = enabled;
    self_test_pending = false;
    self_test_wire_len = 0U;
    self_test_ready_us = 0U;
    metrics.self_test_enabled = enabled;
    metrics.self_test_active = false;
}

bool rs485_engine_self_test_enabled(void)
{
    return self_test_enabled;
}

void rs485_engine_register_external_frame_callback(rs485_engine_external_frame_cb_t cb)
{
    external_frame_cb = cb;
}

uint8_t rs485_engine_node_online(uint8_t node_id)
{
    uint8_t idx = (uint8_t)(node_id - 1U);

    if (idx >= RS485_ENGINE_MAX_NODES)
        return 0U;

    return nodes[idx].state == NODE_STATE_ONLINE;
}

node_state_t rs485_engine_node_state(uint8_t node_id)
{
    uint8_t idx = (uint8_t)(node_id - 1U);

    if (idx >= RS485_ENGINE_MAX_NODES)
        return NODE_STATE_UNKNOWN;

    return nodes[idx].state;
}

void rs485_engine_on_ack(uint8_t node_id, uint16_t msg_id)
{
    (void)msg_id;

    uint8_t idx = (uint8_t)(node_id - 1U);

    if (idx >= RS485_ENGINE_MAX_NODES)
        return;

    nodes[idx].state = NODE_STATE_ONLINE;
    nodes[idx].last_seen_us = (uint64_t)esp_timer_get_time();
    nodes[idx].retry = 0U;
}

void IRAM_ATTR rs485_engine_tick_1ms(void)
{
    if (!engine_enabled)
        return;

    uint64_t now = (uint64_t)esp_timer_get_time();

    for (int i = 0; i < RS485_ENGINE_MAX_NODES; i++)
    {
        if (nodes[i].state != NODE_STATE_ONLINE)
            continue;

        uint64_t diff = now - nodes[i].last_seen_us;

        if (diff > (RS485_ENGINE_TIMEOUT_MS * 1000ULL))
            nodes[i].state = NODE_STATE_OFFLINE;
    }
}

rs485_engine_rx_result_t rs485_engine_receive(rs485_frame_t *frame)
{
    rs485_engine_rx_result_t result;

    if (self_test_enabled && self_test_pending)
    {
        uint64_t now = (uint64_t)esp_timer_get_time();

        if (now >= self_test_ready_us)
        {
            rs485_engine_feed_parser_bounded(self_test_wire, self_test_wire_len);

            if (parser_frame_available())
            {
                self_test_pending = false;
                self_test_wire_len = 0U;
                metrics.self_test_active = false;
            }
        }
    }

    uint16_t bytes_processed = 0U;

    while (rs485_available() && bytes_processed < RS485_ENGINE_RX_BYTES_PER_CALL)
    {
        uint8_t byte = rs485_read_byte();
        bytes_processed++;

        if (parser_process_byte(byte))
            break;
    }

    if (!parser_frame_available())
        return RS485_ENGINE_RX_NONE;

    uint8_t wire[RS485_FRAME_WIRE_MAX];
    int wire_len = parser_get_frame(wire, sizeof(wire));

    if (wire_len <= 0)
    {
        metrics.format_error_count++;
        return RS485_ENGINE_RX_FORMAT_ERROR;
    }

    result = decode_wire_frame(wire, (uint16_t)wire_len, frame);

    switch (result)
    {
        case RS485_ENGINE_RX_FRAME:
            metrics.rx_count++;
            metrics.rx_bytes += (uint32_t)wire_len;
            metrics.last_rx_ms = (uint32_t)(esp_timer_get_time() / 1000ULL);
            break;
        case RS485_ENGINE_RX_CRC_ERROR:
            metrics.crc_error_count++;
            break;
        case RS485_ENGINE_RX_FORMAT_ERROR:
            metrics.format_error_count++;
            break;
        case RS485_ENGINE_RX_NONE:
        default:
            break;
    }

    if (result == RS485_ENGINE_RX_FRAME && frame != NULL)
    {
        if (rs485_engine_should_reply_to_poll(frame))
        {
            rs485_engine_reply_ack(frame);
            return RS485_ENGINE_RX_NONE;
        }

        if (frame->type == RS485_FRAME_TYPE_CLUSTER_HEARTBEAT ||
            frame->type == RS485_FRAME_TYPE_CLUSTER_FRAME)
        {
            uint8_t idx = (uint8_t)(frame->node - 1U);

            if (frame->node != 0U && idx < RS485_ENGINE_MAX_NODES)
            {
                nodes[idx].state = NODE_STATE_ONLINE;
                nodes[idx].last_seen_us = (uint64_t)esp_timer_get_time();
                nodes[idx].retry = 0U;
            }

            if (external_frame_cb)
                external_frame_cb(frame);

            return RS485_ENGINE_RX_NONE;
        }
    }

    return result;
}

void rs485_engine_send(rs485_frame_t *frame)
{
    uint32_t now_ms;

    if (!engine_enabled)
        return;

    if (!frame)
        return;

    now_ms = (uint32_t)(esp_timer_get_time() / 1000ULL);

    if (self_test_enabled &&
        (frame->type == RS485_FRAME_TYPE_POLL) &&
        (frame->node == RS485_ENGINE_SELF_TEST_NODE_ID))
    {
        rs485_frame_t ack = {
            .node = frame->node,
            .msg_id = frame->msg_id,
            .type = RS485_FRAME_TYPE_ACK,
            .len = 0U
        };

        self_test_wire_len = encode_wire_frame(&ack, self_test_wire, sizeof(self_test_wire));
        self_test_ready_us = (uint64_t)esp_timer_get_time() + RS485_ENGINE_SELF_TEST_ACK_DELAY_US;
        self_test_pending = (self_test_wire_len > 0U);
        metrics.tx_count++;
        metrics.tx_bytes += (uint32_t)self_test_wire_len;
        metrics.last_tx_ms = now_ms;
        metrics.self_test_active = self_test_pending;
        return;
    }

    uint8_t wire[RS485_FRAME_WIRE_MAX];
    uint16_t wire_len = encode_wire_frame(frame, wire, sizeof(wire));

    if (wire_len == 0U)
        return;

    rs485_send_bytes(wire, wire_len);
    metrics.tx_count++;
    metrics.tx_bytes += (uint32_t)wire_len;
    metrics.last_tx_ms = now_ms;
}

void rs485_engine_get_metrics(rs485_engine_metrics_t *out_metrics)
{
    if (!out_metrics)
        return;

    metrics.enabled = (engine_enabled != 0U);
    metrics.self_test_enabled = self_test_enabled;
    metrics.self_test_active = self_test_pending;
    *out_metrics = metrics;
}

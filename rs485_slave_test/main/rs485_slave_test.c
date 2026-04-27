#include <ctype.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "driver/uart.h"

#include "parser.h"
#include "rs485.h"

#define TAG "RS485_SLAVE"

#define RS485_SLAVE_NODE_ID        1
#define RS485_SLAVE_UART           UART_NUM_2
#define RS485_SLAVE_TX_PIN         25
#define RS485_SLAVE_RX_PIN         26
#define RS485_SLAVE_DE_PIN         27
#define RS485_SLAVE_BAUDRATE       115200

#define RS485_FRAME_TYPE_POLL      1
#define RS485_FRAME_TYPE_ACK       2

#define RS485_FRAME_MAX_PAYLOAD    64
#define RS485_FRAME_HEADER_SIZE    5
#define RS485_FRAME_CRC_SIZE       2
#define RS485_FRAME_RAW_MAX        (RS485_FRAME_HEADER_SIZE + RS485_FRAME_MAX_PAYLOAD + RS485_FRAME_CRC_SIZE)
#define RS485_FRAME_WIRE_MAX       ((RS485_FRAME_RAW_MAX * 2) + 2)

typedef struct
{
    uint8_t node;
    uint16_t msg_id;
    uint8_t type;
    uint8_t len;
    uint8_t payload[RS485_FRAME_MAX_PAYLOAD];
} slave_frame_t;

static uint16_t crc16_modbus(const uint8_t *data, uint16_t len)
{
    uint16_t crc = 0xFFFF;

    for (uint16_t i = 0; i < len; i++)
    {
        crc ^= data[i];

        for (uint8_t bit = 0; bit < 8; bit++)
        {
            if (crc & 1)
                crc = (crc >> 1) ^ 0xA001;
            else
                crc >>= 1;
        }
    }

    return crc;
}

static char nibble_to_hex(uint8_t value)
{
    value &= 0x0F;
    return (value < 10) ? (char)('0' + value) : (char)('A' + (value - 10));
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

static bool decode_wire_frame(const uint8_t *wire, uint16_t wire_len, slave_frame_t *frame)
{
    uint8_t raw[RS485_FRAME_RAW_MAX];

    if (!wire || !frame)
        return false;

    if ((wire_len == 0) || ((wire_len & 1U) != 0))
        return false;

    uint16_t raw_len = wire_len / 2U;

    if ((raw_len < (RS485_FRAME_HEADER_SIZE + RS485_FRAME_CRC_SIZE)) || (raw_len > sizeof(raw)))
        return false;

    for (uint16_t i = 0; i < raw_len; i++)
    {
        int hi = hex_to_nibble(wire[i * 2]);
        int lo = hex_to_nibble(wire[(i * 2) + 1]);

        if ((hi < 0) || (lo < 0))
            return false;

        raw[i] = (uint8_t)((hi << 4) | lo);
    }

    uint8_t payload_len = raw[4];
    uint16_t expected_len = RS485_FRAME_HEADER_SIZE + payload_len + RS485_FRAME_CRC_SIZE;

    if ((expected_len != raw_len) || (payload_len > RS485_FRAME_MAX_PAYLOAD))
        return false;

    uint16_t expected_crc = (uint16_t)((raw[expected_len - 2] << 8) | raw[expected_len - 1]);
    uint16_t actual_crc = crc16_modbus(raw, expected_len - RS485_FRAME_CRC_SIZE);

    if (expected_crc != actual_crc)
        return false;

    frame->node = raw[0];
    frame->msg_id = (uint16_t)((raw[1] << 8) | raw[2]);
    frame->type = raw[3];
    frame->len = payload_len;

    if (payload_len)
        memcpy(frame->payload, &raw[5], payload_len);

    return true;
}

static uint16_t encode_wire_frame(const slave_frame_t *frame, uint8_t *wire, uint16_t max_len)
{
    uint8_t raw[RS485_FRAME_RAW_MAX];

    if (!frame || !wire)
        return 0;

    if (frame->len > RS485_FRAME_MAX_PAYLOAD)
        return 0;

    uint16_t raw_len = RS485_FRAME_HEADER_SIZE + frame->len + RS485_FRAME_CRC_SIZE;
    uint16_t wire_len = (raw_len * 2U) + 2U;

    if (wire_len > max_len)
        return 0;

    raw[0] = frame->node;
    raw[1] = (uint8_t)(frame->msg_id >> 8);
    raw[2] = (uint8_t)(frame->msg_id & 0xFF);
    raw[3] = frame->type;
    raw[4] = frame->len;

    if (frame->len)
        memcpy(&raw[5], frame->payload, frame->len);

    uint16_t crc = crc16_modbus(raw, RS485_FRAME_HEADER_SIZE + frame->len);
    raw[5 + frame->len] = (uint8_t)(crc >> 8);
    raw[6 + frame->len] = (uint8_t)(crc & 0xFF);

    wire[0] = '<';

    for (uint16_t i = 0; i < raw_len; i++)
    {
        wire[1 + (i * 2)] = (uint8_t)nibble_to_hex(raw[i] >> 4);
        wire[2 + (i * 2)] = (uint8_t)nibble_to_hex(raw[i]);
    }

    wire[wire_len - 1] = '>';

    return wire_len;
}

static bool receive_frame(slave_frame_t *frame)
{
    while (rs485_available())
    {
        uint8_t byte = rs485_read_byte();

        if (parser_process_byte(byte))
            break;
    }

    if (!parser_frame_available())
        return false;

    uint8_t wire[RS485_FRAME_WIRE_MAX];
    int len = parser_get_frame(wire, sizeof(wire));

    if (len <= 0)
        return false;

    return decode_wire_frame(wire, (uint16_t)len, frame);
}

static void send_ack(uint8_t node_id, uint16_t msg_id)
{
    slave_frame_t ack = {
        .node = node_id,
        .msg_id = msg_id,
        .type = RS485_FRAME_TYPE_ACK,
        .len = 0
    };

    uint8_t wire[RS485_FRAME_WIRE_MAX];
    uint16_t wire_len = encode_wire_frame(&ack, wire, sizeof(wire));

    if (wire_len == 0)
        return;

    rs485_send_bytes(wire, wire_len);
}

void app_main(void)
{
    parser_init();

    rs485_init(&(rs485_config_t) {
        .uart_num = RS485_SLAVE_UART,
        .tx_pin = RS485_SLAVE_TX_PIN,
        .rx_pin = RS485_SLAVE_RX_PIN,
        .de_pin = RS485_SLAVE_DE_PIN,
        .baudrate = RS485_SLAVE_BAUDRATE
    });

    ESP_LOGI(TAG,
        "Slave online node=%u uart=%d tx=%d rx=%d de=%d baud=%d",
        RS485_SLAVE_NODE_ID,
        RS485_SLAVE_UART,
        RS485_SLAVE_TX_PIN,
        RS485_SLAVE_RX_PIN,
        RS485_SLAVE_DE_PIN,
        RS485_SLAVE_BAUDRATE);

    while (1)
    {
        slave_frame_t frame = {0};

        if (!receive_frame(&frame))
        {
            vTaskDelay(pdMS_TO_TICKS(1));
            continue;
        }

        if (frame.node != RS485_SLAVE_NODE_ID)
            continue;

        if (frame.type != RS485_FRAME_TYPE_POLL)
            continue;

        int64_t t0 = esp_timer_get_time();
        send_ack(frame.node, frame.msg_id);
        int64_t dt = esp_timer_get_time() - t0;

        ESP_LOGI(TAG,
            "ACK node=%u msg=%u tx_time=%" PRIi64 " us",
            frame.node,
            frame.msg_id,
            dt);
    }
}

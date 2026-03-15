#include "rs485.h"
#include "driver/uart.h"
#include "driver/gpio.h"
#include "esp_log.h"

#define TAG "RS485"
#define RX_BUFFER_SIZE 256

static rs485_config_t cfg;

static uint8_t rx_buffer[RX_BUFFER_SIZE];
static volatile uint16_t rx_head = 0;
static volatile uint16_t rx_tail = 0;



/* ============================
   CONTROLE DIREÇÃO RS485
   ============================ */

void rs485_set_tx_mode(void)
{
    gpio_set_level(cfg.de_pin, 1);
}

void rs485_set_rx_mode(void)
{
    gpio_set_level(cfg.de_pin, 0);
}



/* ============================
   INIT
   ============================ */

void rs485_init(const rs485_config_t *config)
{
    cfg = *config;

    uart_config_t uart_config =
    {
        .baud_rate = cfg.baudrate,
        .data_bits = UART_DATA_8_BITS,
        .parity    = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_APB,
    };

    uart_driver_install(cfg.uart_num, 512, 512, 0, NULL, 0);
    uart_param_config(cfg.uart_num, &uart_config);
    uart_set_pin(cfg.uart_num, cfg.tx_pin, cfg.rx_pin, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);

    gpio_config_t io_conf =
    {
        .pin_bit_mask = 1ULL << cfg.de_pin,
        .mode = GPIO_MODE_OUTPUT
    };
    gpio_config(&io_conf);

    rs485_set_rx_mode();

    ESP_LOGI(TAG, "Driver RS485 inicializado");
}



/* ============================
   TX
   ============================ */

void rs485_send_bytes(const uint8_t *data, uint16_t len)
{
    rs485_set_tx_mode();

    uart_write_bytes(cfg.uart_num, (const char*)data, len);
    uart_wait_tx_done(cfg.uart_num, pdMS_TO_TICKS(2));

    rs485_set_rx_mode();
}



/* ============================
   RX LOW LEVEL
   ============================ */

static void rs485_poll_rx(void)
{
    uint8_t data[64];

    int len = uart_read_bytes(cfg.uart_num, data, sizeof(data), 0);

    for(int i = 0; i < len; i++)
    {
        uint16_t next = (rx_head + 1) % RX_BUFFER_SIZE;

        if(next != rx_tail)
        {
            rx_buffer[rx_head] = data[i];
            rx_head = next;
        }
    }
}



/* ============================
   RX API
   ============================ */

bool rs485_available(void)
{
    rs485_poll_rx();
    return rx_head != rx_tail;
}

uint8_t rs485_read_byte(void)
{
    if(rx_head == rx_tail)
        return 0;

    uint8_t b = rx_buffer[rx_tail];
    rx_tail = (rx_tail + 1) % RX_BUFFER_SIZE;
    return b;
}

int rs485_read_frame(uint8_t *buf, int max_len)
{
    rs485_poll_rx();

    int count = 0;

    while(rx_head != rx_tail && count < max_len)
    {
        buf[count++] = rs485_read_byte();
    }

    return count;
}


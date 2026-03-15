#include "hal_uart.h"
#include "driver/uart.h"
#include "esp_log.h"

static const char *TAG = "HAL_UART";

static int current_uart = UART_NUM_1;

void hal_uart_init(const hal_uart_config_t *cfg)
{
    current_uart = cfg->uart_num;

    uart_config_t config =
    {
        .baud_rate = cfg->baudrate,
        .data_bits = UART_DATA_8_BITS,
        .parity    = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE
    };

    uart_driver_install(current_uart, 1024, 0, 0, NULL, 0);
    uart_param_config(current_uart, &config);
    uart_set_pin(current_uart, cfg->tx_pin, cfg->rx_pin,
                 UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);

    ESP_LOGI(TAG, "UART inicializada");
}

int hal_uart_write(const uint8_t *data, uint16_t len)
{
    return uart_write_bytes(current_uart, (const char *)data, len);
}

int hal_uart_read(uint8_t *buffer, uint16_t maxlen)
{
    return uart_read_bytes(current_uart, buffer, maxlen, 0);
}

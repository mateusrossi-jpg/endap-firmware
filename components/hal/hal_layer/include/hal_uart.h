#pragma once
#include <stdint.h>
#include <stdbool.h>

typedef struct
{
    int uart_num;
    int tx_pin;
    int rx_pin;
    int baudrate;
} hal_uart_config_t;

void hal_uart_init(const hal_uart_config_t *cfg);
int  hal_uart_write(const uint8_t *data, uint16_t len);
int  hal_uart_read(uint8_t *buffer, uint16_t maxlen);

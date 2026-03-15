#pragma once
#include <stdint.h>
#include <stdbool.h>

typedef struct
{
    int uart_num;
    int tx_pin;
    int rx_pin;
    int de_pin;
    int baudrate;
} rs485_config_t;

void rs485_init(const rs485_config_t *cfg);

void rs485_send_bytes(const uint8_t *data, uint16_t len);

int rs485_read_bytes(uint8_t *buffer, uint16_t max_len);

void rs485_set_tx_mode(void);
void rs485_set_rx_mode(void);

bool rs485_available(void);
uint8_t rs485_read_byte(void);
int rs485_read_frame(uint8_t *buf, int max_len);



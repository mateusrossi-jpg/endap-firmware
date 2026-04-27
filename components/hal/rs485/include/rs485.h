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

    /* false = DE externo; true = auto-direction (TTL485-V2.0) */
    bool auto_direction;

    /* Guardas opcionais; se 0, o HAL aplica defaults conforme o modo. */
    uint32_t tx_guard_us;
    uint32_t rx_recovery_us;
    uint32_t tx_done_timeout_ms;
} rs485_config_t;

typedef struct
{
    bool initialized;
    bool auto_direction;
    uint32_t tx_bytes;
    uint32_t rx_bytes;
    uint32_t rx_dropped_bytes;
    uint32_t tx_errors;
    uint32_t last_tx_ms;
    uint32_t last_rx_ms;

    /* Telemetria extra para diagnostico de modulos auto-direction */
    uint32_t rx_poll_calls;
    uint32_t rx_chunks;
    uint32_t max_rx_chunk;
    uint32_t last_rx_chunk;
    uint32_t rx_reads_while_tx_window;
} rs485_hal_metrics_t;

void rs485_init(const rs485_config_t *cfg);

void rs485_send_bytes(const uint8_t *data, uint16_t len);
int rs485_read_bytes(uint8_t *buffer, uint16_t max_len);

void rs485_set_tx_mode(void);
void rs485_set_rx_mode(void);

bool rs485_available(void);
uint8_t rs485_read_byte(void);
int rs485_read_frame(uint8_t *buf, int max_len);
void rs485_get_metrics(rs485_hal_metrics_t *out_metrics);

/* suporte para TX nao bloqueante */
void rs485_tx_service(void);
bool rs485_tx_busy(void);

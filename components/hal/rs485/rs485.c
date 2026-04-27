#include "rs485.h"

#include "driver/gpio.h"
#include "driver/uart.h"
#include "esp_intr_alloc.h"
#include "esp_log.h"
#include "esp_timer.h"

#include "freertos/FreeRTOS.h"
#include "freertos/portmacro.h"
#include "freertos/task.h"

#include <string.h>
#include <inttypes.h>

#define TAG "RS485"
#define RX_BUFFER_SIZE 256
#define UART_RX_DRIVER_BUFFER_SIZE 1024

#define RS485_DEFAULT_TX_DONE_TIMEOUT_MS 20U
#define RS485_DEFAULT_TX_GUARD_US        100U
#define RS485_DEFAULT_RX_RECOVERY_US     0U

/* Ajustes mais folgados para placas auto-direction como TTL485-V2.0 */
#define RS485_AUTO_TX_GUARD_US           3000U
#define RS485_AUTO_RX_RECOVERY_US        2000U

typedef struct
{
    bool active;
    uint64_t release_deadline_us;
    uint64_t rx_release_deadline_us;
    uint16_t pending_len;
} rs485_tx_state_t;

static rs485_config_t cfg;
static rs485_hal_metrics_t metrics;
static bool driver_installed = false;
static bool de_pin_configured = false;
static portMUX_TYPE rs485_lock = portMUX_INITIALIZER_UNLOCKED;

static uint8_t rx_buffer[RX_BUFFER_SIZE];
static volatile uint16_t rx_head = 0;
static volatile uint16_t rx_tail = 0;
static rs485_tx_state_t tx_state = {0};

static bool rs485_has_valid_uart(const rs485_config_t *config)
{
    return config && config->uart_num >= UART_NUM_0 && config->uart_num < UART_NUM_MAX;
}

static bool rs485_has_valid_data_pins(const rs485_config_t *config)
{
    return config && config->tx_pin >= 0 && config->rx_pin >= 0;
}

static bool rs485_uses_external_direction(void)
{
    return !cfg.auto_direction && cfg.de_pin >= 0;
}

static void rs485_apply_defaults(rs485_config_t *config)
{
    if (!config)
        return;

    if (config->auto_direction)
        config->de_pin = -1;

    if (config->tx_guard_us == 0U)
        config->tx_guard_us = config->auto_direction ? RS485_AUTO_TX_GUARD_US : RS485_DEFAULT_TX_GUARD_US;

    if (config->rx_recovery_us == 0U)
        config->rx_recovery_us = config->auto_direction ? RS485_AUTO_RX_RECOVERY_US : RS485_DEFAULT_RX_RECOVERY_US;

    if (config->tx_done_timeout_ms == 0U)
        config->tx_done_timeout_ms = RS485_DEFAULT_TX_DONE_TIMEOUT_MS;
}

static uint32_t rs485_estimate_tx_time_us(uint16_t len)
{
    if (cfg.baudrate <= 0 || len == 0U)
        return 0U;

    /* 8N1 -> ~10 bits por byte */
    uint64_t bits = (uint64_t)len * 10ULL;
    uint64_t us = (bits * 1000000ULL + (uint64_t)cfg.baudrate - 1ULL) / (uint64_t)cfg.baudrate;
    us += (uint64_t)cfg.tx_guard_us;

    if (us > UINT32_MAX)
        us = UINT32_MAX;

    return (uint32_t)us;
}

/* ============================
   CONTROLE DIRECAO RS485
   ============================ */

void rs485_set_tx_mode(void)
{
    if (de_pin_configured && rs485_uses_external_direction())
        gpio_set_level((gpio_num_t)cfg.de_pin, 1);
}

void rs485_set_rx_mode(void)
{
    if (de_pin_configured && rs485_uses_external_direction())
        gpio_set_level((gpio_num_t)cfg.de_pin, 0);
}

/* ============================
   TX SERVICE
   ============================ */

bool rs485_tx_busy(void)
{
    return tx_state.active;
}

void rs485_tx_service(void)
{
    if (!metrics.initialized || !tx_state.active)
        return;

    uint64_t now = (uint64_t)esp_timer_get_time();

    if (now < tx_state.release_deadline_us)
        return;

    TickType_t wait_ticks = pdMS_TO_TICKS(cfg.tx_done_timeout_ms);
    esp_err_t done = uart_wait_tx_done(cfg.uart_num, wait_ticks);

    if (done == ESP_OK)
    {
        rs485_set_rx_mode();
        tx_state.active = false;
        tx_state.pending_len = 0U;
        tx_state.release_deadline_us = 0U;
        tx_state.rx_release_deadline_us = 0U;
        return;
    }

    /* Modulos auto-direction baratos podem atrasar a troca de direcao.
       Se o prazo total estourou, libera RX mesmo assim e contabiliza erro. */
    if (now >= (tx_state.release_deadline_us + ((uint64_t)cfg.tx_done_timeout_ms * 1000ULL)))
    {
        metrics.tx_errors++;
        rs485_set_rx_mode();
        tx_state.active = false;
        tx_state.pending_len = 0U;
        tx_state.release_deadline_us = 0U;
        tx_state.rx_release_deadline_us = 0U;
    }
}

/* ============================
   INIT
   ============================ */

void rs485_init(const rs485_config_t *config)
{
    esp_err_t err;
    rs485_config_t local_cfg;

    if (!config || !rs485_has_valid_uart(config) || !rs485_has_valid_data_pins(config) || config->baudrate <= 0)
    {
        ESP_LOGE(TAG, "Config RS485 invalida");
        return;
    }

    if (driver_installed)
    {
        ESP_LOGW(TAG, "Driver RS485 ja inicializado");
        return;
    }

    local_cfg = *config;
    rs485_apply_defaults(&local_cfg);
    cfg = local_cfg;

    memset(&metrics, 0, sizeof(metrics));
    metrics.auto_direction = cfg.auto_direction;
    rx_head = 0;
    rx_tail = 0;
    tx_state.active = false;
    tx_state.release_deadline_us = 0U;
    tx_state.rx_release_deadline_us = 0U;
    tx_state.pending_len = 0U;

    uart_config_t uart_config = {
        .baud_rate = cfg.baudrate,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };

    err = uart_param_config(cfg.uart_num, &uart_config);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "Falha em uart_param_config: %s", esp_err_to_name(err));
        return;
    }

    err = uart_set_pin(cfg.uart_num,
                       cfg.tx_pin,
                       cfg.rx_pin,
                       UART_PIN_NO_CHANGE,
                       UART_PIN_NO_CHANGE);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "Falha em uart_set_pin: %s", esp_err_to_name(err));
        return;
    }

    err = uart_driver_install(cfg.uart_num,
                              UART_RX_DRIVER_BUFFER_SIZE,
                              0,
                              0,
                              NULL,
                              ESP_INTR_FLAG_IRAM);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "Falha em uart_driver_install: %s", esp_err_to_name(err));
        return;
    }

    driver_installed = true;

    err = uart_set_mode(cfg.uart_num, UART_MODE_UART);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "Falha em uart_set_mode: %s", esp_err_to_name(err));
        uart_driver_delete(cfg.uart_num);
        driver_installed = false;
        return;
    }

    if (rs485_uses_external_direction())
    {
        gpio_config_t io_cfg = {
            .pin_bit_mask = 1ULL << (uint64_t)cfg.de_pin,
            .mode = GPIO_MODE_OUTPUT,
            .pull_up_en = GPIO_PULLUP_DISABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type = GPIO_INTR_DISABLE,
        };

        err = gpio_config(&io_cfg);
        if (err != ESP_OK)
        {
            ESP_LOGE(TAG, "Falha ao configurar DE do RS485: %s", esp_err_to_name(err));
            uart_driver_delete(cfg.uart_num);
            driver_installed = false;
            return;
        }

        de_pin_configured = true;
        rs485_set_rx_mode();
    }
    else
    {
        de_pin_configured = false;
    }

    uart_flush_input(cfg.uart_num);
    metrics.initialized = true;

    ESP_LOGI(TAG,
             "Driver RS485 inicializado (uart=%d tx=%d rx=%d de=%d baud=%d auto=%d guard=%" PRIu32 "us rx_recovery=%" PRIu32 "us)",
             cfg.uart_num,
             cfg.tx_pin,
             cfg.rx_pin,
             cfg.de_pin,
             cfg.baudrate,
             cfg.auto_direction ? 1 : 0,
             cfg.tx_guard_us,
             cfg.rx_recovery_us);
}

/* ============================
   TX
   ============================ */

void rs485_send_bytes(const uint8_t *data, uint16_t len)
{
    int written;
    uint64_t now_us;
    uint32_t tx_time_us;

    if (!metrics.initialized || !data || len == 0U)
        return;

    rs485_tx_service();

    if (tx_state.active)
    {
        metrics.tx_errors++;
        return;
    }

    rs485_set_tx_mode();

    written = uart_write_bytes(cfg.uart_num, (const char *)data, len);
    if (written < 0)
    {
        metrics.tx_errors++;
        rs485_set_rx_mode();
        return;
    }

    now_us = (uint64_t)esp_timer_get_time();
    tx_time_us = rs485_estimate_tx_time_us((uint16_t)written);

    tx_state.active = true;
    tx_state.pending_len = (uint16_t)written;
    tx_state.release_deadline_us = now_us + (uint64_t)tx_time_us;
    tx_state.rx_release_deadline_us = tx_state.release_deadline_us + (uint64_t)cfg.rx_recovery_us;

    metrics.tx_bytes += (uint32_t)written;
    metrics.last_tx_ms = (uint32_t)(now_us / 1000ULL);
}

/* ============================
   RX LOW LEVEL
   ============================ */

static void rs485_poll_rx(void)
{
    uint8_t data[64];
    int len;
    uint64_t now_us;

    if (!metrics.initialized)
        return;

    metrics.rx_poll_calls++;
    now_us = (uint64_t)esp_timer_get_time();

    if (!cfg.auto_direction && tx_state.active)
        return;

    if (cfg.auto_direction && now_us < tx_state.rx_release_deadline_us)
    {
        metrics.rx_reads_while_tx_window++;
        return;
    }

    len = uart_read_bytes(cfg.uart_num, data, sizeof(data), 0);
    if (len <= 0)
        return;

    metrics.rx_chunks++;
    metrics.last_rx_chunk = (uint32_t)len;
    if ((uint32_t)len > metrics.max_rx_chunk)
        metrics.max_rx_chunk = (uint32_t)len;

    portENTER_CRITICAL(&rs485_lock);

    for (int i = 0; i < len; i++)
    {
        uint16_t next = (uint16_t)((rx_head + 1U) % RX_BUFFER_SIZE);

        if (next != rx_tail)
        {
            rx_buffer[rx_head] = data[i];
            rx_head = next;
        }
        else
        {
            metrics.rx_dropped_bytes++;
        }
    }

    portEXIT_CRITICAL(&rs485_lock);

    metrics.rx_bytes += (uint32_t)len;
    metrics.last_rx_ms = (uint32_t)(esp_timer_get_time() / 1000ULL);
}

/* ============================
   RX API
   ============================ */

bool rs485_available(void)
{
    rs485_tx_service();
    rs485_poll_rx();
    return rx_head != rx_tail;
}

uint8_t rs485_read_byte(void)
{
    uint8_t b = 0;

    portENTER_CRITICAL(&rs485_lock);

    if (rx_head != rx_tail)
    {
        b = rx_buffer[rx_tail];
        rx_tail = (uint16_t)((rx_tail + 1U) % RX_BUFFER_SIZE);
    }

    portEXIT_CRITICAL(&rs485_lock);

    return b;
}

int rs485_read_frame(uint8_t *buf, int max_len)
{
    int count = 0;

    if (!buf || max_len <= 0)
        return 0;

    rs485_tx_service();
    rs485_poll_rx();

    while (rx_head != rx_tail && count < max_len)
        buf[count++] = rs485_read_byte();

    return count;
}

int rs485_read_bytes(uint8_t *buffer, uint16_t max_len)
{
    uint16_t count = 0;

    if (!buffer || max_len == 0U)
        return 0;

    rs485_tx_service();
    rs485_poll_rx();

    while ((rx_head != rx_tail) && (count < max_len))
        buffer[count++] = rs485_read_byte();

    return (int)count;
}

void rs485_get_metrics(rs485_hal_metrics_t *out_metrics)
{
    if (!out_metrics)
        return;

    *out_metrics = metrics;
}

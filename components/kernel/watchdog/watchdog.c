#include "watchdog.h"

#include <inttypes.h>
#include <limits.h>

#include "esp_log.h"
#include "esp_system.h"

#include "freertos/FreeRTOS.h"
#include "freertos/portmacro.h"
#include "freertos/task.h"

#define TAG "WATCHDOG"
#define MAX_WD 16
#define WATCHDOG_MAX_MISSES 3U

typedef struct
{
    TickType_t timeout_ticks;
    TickType_t last_feed_tick;
    uint8_t miss_count;
    bool active;

} wd_entry_t;

static wd_entry_t table[MAX_WD];
static portMUX_TYPE watchdog_lock = portMUX_INITIALIZER_UNLOCKED;

void watchdog_init(void)
{
    portENTER_CRITICAL(&watchdog_lock);

    for (int i = 0; i < MAX_WD; i++)
    {
        table[i].active = false;
        table[i].timeout_ticks = 0;
        table[i].last_feed_tick = 0;
        table[i].miss_count = 0;
    }

    portEXIT_CRITICAL(&watchdog_lock);

    ESP_LOGI(TAG, "Watchdog initialized");
}

void watchdog_register(uint8_t id, uint32_t timeout_ms)
{
    TickType_t now_ticks;
    TickType_t timeout_ticks;

    if (id >= MAX_WD)
        return;

    now_ticks = xTaskGetTickCount();
    timeout_ticks = pdMS_TO_TICKS(timeout_ms);

    if (timeout_ticks == 0)
        timeout_ticks = 1;

    portENTER_CRITICAL(&watchdog_lock);

    table[id].timeout_ticks = timeout_ticks;
    table[id].last_feed_tick = now_ticks;
    table[id].miss_count = 0;
    table[id].active = true;

    portEXIT_CRITICAL(&watchdog_lock);

    ESP_LOGI(TAG, "Registered WD %u", id);
}

void watchdog_feed(uint8_t id)
{
    TickType_t now_ticks;

    if (id >= MAX_WD)
        return;

    now_ticks = xTaskGetTickCount();

    portENTER_CRITICAL(&watchdog_lock);

    if (table[id].active)
    {
        table[id].last_feed_tick = now_ticks;
        table[id].miss_count = 0;
    }

    portEXIT_CRITICAL(&watchdog_lock);
}

void watchdog_check(void)
{
    for (int i = 0; i < MAX_WD; i++)
    {
        TickType_t timeout_ticks = 0;
        TickType_t last_feed_tick = 0;
        TickType_t elapsed = 0;
        uint8_t new_miss_count = 0;
        bool active = false;
        bool warn = false;
        bool restart_needed = false;

        portENTER_CRITICAL(&watchdog_lock);

        active = table[i].active;

        if (active)
        {
            timeout_ticks = table[i].timeout_ticks;
            last_feed_tick = table[i].last_feed_tick;
            /* Leitura do tick deve acontecer junto do snapshot protegido.
               Caso contrario, outro core pode alimentar o watchdog logo
               depois da amostragem e antes desta subtracao, gerando
               underflow espurio e falso positivo. */
            elapsed = xTaskGetTickCount() - last_feed_tick;

            if (elapsed > timeout_ticks)
            {
                if (table[i].miss_count < UINT8_MAX)
                    table[i].miss_count++;

                new_miss_count = table[i].miss_count;

                if (new_miss_count == 1U)
                    warn = true;

                if (new_miss_count >= WATCHDOG_MAX_MISSES)
                    restart_needed = true;
            }
            else
            {
                table[i].miss_count = 0;
            }
        }

        portEXIT_CRITICAL(&watchdog_lock);

        if (!active)
            continue;

        if (warn)
        {
            ESP_LOGW(TAG,
                "WATCHDOG atraso detectado task=%d elapsed_ticks=%" PRIu32 " timeout_ticks=%" PRIu32,
                i,
                (uint32_t)elapsed,
                (uint32_t)timeout_ticks);
        }

        if (restart_needed)
        {
            bool still_expired = false;
            uint8_t confirmed_miss_count = 0;
            TickType_t confirm_timeout_ticks = 0;
            TickType_t confirm_elapsed = 0;

            portENTER_CRITICAL(&watchdog_lock);

            if (table[i].active)
            {
                confirm_timeout_ticks = table[i].timeout_ticks;
                confirm_elapsed = xTaskGetTickCount() - table[i].last_feed_tick;
                confirmed_miss_count = table[i].miss_count;

                if ((confirm_elapsed > confirm_timeout_ticks) &&
                    (confirmed_miss_count >= WATCHDOG_MAX_MISSES))
                {
                    still_expired = true;
                }
            }

            portEXIT_CRITICAL(&watchdog_lock);

            if (still_expired)
            {
                ESP_LOGE(TAG,
                    "WATCHDOG TIMEOUT task=%d elapsed_ticks=%" PRIu32 " timeout_ticks=%" PRIu32 " misses=%u",
                    i,
                    (uint32_t)confirm_elapsed,
                    (uint32_t)confirm_timeout_ticks,
                    (unsigned)confirmed_miss_count);
                esp_restart();
            }
        }
    }
}

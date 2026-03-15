#include "fieldbus.h"

#include "rs485_master.h"
#include "esp_timer.h"
#include "latency_histogram.h"

static uint64_t bus_last_activity = 0;
static uint32_t histogram_cycle = 0;

/* ========================================================= */

void fieldbus_init(void)
{
    rs485_master_init();

    latency_histogram_init();

    bus_last_activity = esp_timer_get_time();
}

/* ========================================================= */

static void bus_watchdog(void)
{
    uint64_t now = esp_timer_get_time();

    if((now - bus_last_activity) > 2000000)
    {
        /* apenas marca condição */
        bus_last_activity = now;
    }
}

/* ========================================================= */

void fieldbus_tick(void)
{
    uint64_t start = esp_timer_get_time();

    rs485_master_tick();

    uint32_t latency = esp_timer_get_time() - start;

    latency_histogram_record(latency);

    histogram_cycle++;

    /* log a cada ~10 segundos */

    if(histogram_cycle >= 10000)
    {
        latency_histogram_log();
        histogram_cycle = 0;
    }

    bus_last_activity = esp_timer_get_time();

    bus_watchdog();
}

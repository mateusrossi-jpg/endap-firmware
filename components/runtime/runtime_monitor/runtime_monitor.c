#include "runtime_monitor.h"

#include "esp_timer.h"

static uint64_t last_cycle = 0;
static uint32_t last_cycle_us = 0;
static uint8_t fault = 0;

/* ========================================================= */

void runtime_monitor_init(void)
{
    last_cycle = esp_timer_get_time();
}

/* ========================================================= */

void runtime_monitor_tick(void)
{
    uint64_t now = esp_timer_get_time();

    last_cycle_us = (uint32_t)(now - last_cycle);

    last_cycle = now;

    if(last_cycle_us > 2000)
        fault = 1;
    else
        fault = 0;
}

/* ========================================================= */

uint32_t runtime_monitor_get_last_cycle_us(void)
{
    return last_cycle_us;
}

uint8_t runtime_monitor_is_fault(void)
{
    return fault;
}

#include "bus_health_monitor.h"
#include <string.h>

static bus_health_metrics_t metrics;

static uint64_t latency_sum = 0;
static uint32_t latency_count = 0;


/* ============================= */
/* INIT                          */
/* ============================= */

void bus_health_init(void)
{
    memset(&metrics, 0, sizeof(metrics));

    latency_sum = 0;
    latency_count = 0;
}


/* ============================= */
/* EVENTS                        */
/* ============================= */

void bus_health_crc_error(void)
{
    metrics.crc_errors++;
}

void bus_health_timeout(void)
{
    metrics.timeouts++;
}

void bus_health_retry(void)
{
    metrics.retries++;
}


/* ============================= */
/* LATENCY TRACKING              */
/* ============================= */

void bus_health_latency(uint32_t latency)
{
    latency_sum += latency;
    latency_count++;

    if (latency > metrics.max_latency_us)
        metrics.max_latency_us = latency;

    if (latency_count)
        metrics.avg_latency_us = latency_sum / latency_count;
}


/* ============================= */
/* GET METRICS                   */
/* ============================= */

void bus_health_get(bus_health_metrics_t *m)
{
    if (!m)
        return;

    *m = metrics;
}

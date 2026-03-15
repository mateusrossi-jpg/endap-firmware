#ifndef ENDAP_BUS_HEALTH_MONITOR_H
#define ENDAP_BUS_HEALTH_MONITOR_H

#include <stdint.h>

typedef struct
{
    uint32_t crc_errors;
    uint32_t timeouts;
    uint32_t retries;

    uint32_t avg_latency_us;
    uint32_t max_latency_us;

} bus_health_metrics_t;

void bus_health_init(void);

void bus_health_crc_error(void);
void bus_health_timeout(void);
void bus_health_retry(void);

void bus_health_latency(uint32_t latency);

void bus_health_get(bus_health_metrics_t *m);

#endif

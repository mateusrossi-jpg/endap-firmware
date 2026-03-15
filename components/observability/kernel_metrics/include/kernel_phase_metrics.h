#ifndef ENDAP_KERNEL_PHASE_METRICS_H
#define ENDAP_KERNEL_PHASE_METRICS_H

#include <stdint.h>

typedef struct
{
    uint32_t io_max;
    uint32_t fieldbus_max;
    uint32_t automation_max;
    uint32_t events_max;
    uint32_t diagnostics_max;

} kernel_phase_metrics_t;

void kernel_phase_metrics_update(
    uint32_t io,
    uint32_t fieldbus,
    uint32_t automation,
    uint32_t events,
    uint32_t diagnostics
);

void kernel_phase_metrics_get(kernel_phase_metrics_t *m);

#endif

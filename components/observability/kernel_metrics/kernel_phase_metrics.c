#include "kernel_phase_metrics.h"

static kernel_phase_metrics_t phase_metrics;

void kernel_phase_metrics_update(
    uint32_t io,
    uint32_t fieldbus,
    uint32_t automation,
    uint32_t events,
    uint32_t diagnostics
)
{
    if(io > phase_metrics.io_max)
        phase_metrics.io_max = io;

    if(fieldbus > phase_metrics.fieldbus_max)
        phase_metrics.fieldbus_max = fieldbus;

    if(automation > phase_metrics.automation_max)
        phase_metrics.automation_max = automation;

    if(events > phase_metrics.events_max)
        phase_metrics.events_max = events;

    if(diagnostics > phase_metrics.diagnostics_max)
        phase_metrics.diagnostics_max = diagnostics;
}

void kernel_phase_metrics_get(kernel_phase_metrics_t *m)
{
    if(!m)
        return;

    *m = phase_metrics;
}

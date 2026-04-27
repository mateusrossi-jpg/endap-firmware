#pragma once

#include <stdint.h>

typedef struct
{
    uint32_t io_deadline_us;
    uint32_t io_apply_deadline_us;
    uint32_t fieldbus_deadline_us;
    uint32_t automation_deadline_us;
    uint32_t events_deadline_us;
    uint32_t diagnostics_deadline_us;

    uint32_t io_overruns;
    uint32_t io_apply_overruns;
    uint32_t fieldbus_overruns;
    uint32_t automation_overruns;
    uint32_t events_overruns;
    uint32_t diagnostics_overruns;
} phase_monitor_snapshot_t;

void phase_monitor_check(
    uint8_t phase,
    uint32_t exec_time
);

void phase_monitor_get(phase_monitor_snapshot_t *snapshot);

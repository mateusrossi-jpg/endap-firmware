#pragma once

#include <stdint.h>

typedef struct
{
    uint64_t max_jitter;
    uint64_t max_exec_time;

    uint32_t deadline_miss;
    uint32_t overrun_count;

} control_loop_metrics_t;

void control_loop_get_metrics(control_loop_metrics_t *m);

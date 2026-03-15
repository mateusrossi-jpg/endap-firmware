#ifndef ENDAP_KERNEL_METRICS_API_H
#define ENDAP_KERNEL_METRICS_API_H

#include <stdint.h>

typedef struct
{
    uint64_t max_jitter;
    uint64_t max_exec_time;

    uint32_t deadline_miss;
    uint32_t overrun_count;

} kernel_metrics_snapshot_t;

void kernel_metrics_get(kernel_metrics_snapshot_t *m);

#endif

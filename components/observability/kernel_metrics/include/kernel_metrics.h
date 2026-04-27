#ifndef ENDAP_KERNEL_METRICS_H
#define ENDAP_KERNEL_METRICS_H

#include <stdint.h>

typedef struct
{
    uint64_t max_jitter;
    uint64_t max_exec_time;
    uint32_t deadline_miss;
    uint32_t overrun_count;
} kernel_metrics_snapshot_t;

/* API pública */
void kernel_metrics_init(void);

void kernel_metrics_update(
    uint64_t jitter,
    uint64_t exec,
    uint32_t miss,
    uint32_t over);

void kernel_metrics_get(kernel_metrics_snapshot_t *m);

void kernel_metrics_log(void);

#endif

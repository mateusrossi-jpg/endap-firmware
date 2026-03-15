#include "kernel_metrics_api.h"
#include "kernel_metrics.h"

extern uint64_t kernel_max_jitter;
extern uint64_t kernel_max_exec;
extern uint32_t kernel_deadline_miss;
extern uint32_t kernel_overrun;

void kernel_metrics_get(kernel_metrics_snapshot_t *m)
{
    if(!m)
        return;

    m->max_jitter = kernel_max_jitter;
    m->max_exec_time = kernel_max_exec;
    m->deadline_miss = kernel_deadline_miss;
    m->overrun_count = kernel_overrun;
}

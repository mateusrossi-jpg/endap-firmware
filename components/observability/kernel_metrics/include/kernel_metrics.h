#pragma once

#include <stdint.h>

void kernel_metrics_init(void);

void kernel_metrics_update(
    uint64_t jitter_max,
    uint64_t exec_max,
    uint32_t deadline_miss,
    uint32_t overrun
);

void kernel_metrics_log(void);

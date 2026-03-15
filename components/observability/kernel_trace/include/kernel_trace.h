#ifndef KERNEL_TRACE_H
#define KERNEL_TRACE_H

#include <stdint.h>
#include "esp_attr.h"

void IRAM_ATTR kernel_trace_cycle_start(uint64_t now);
void IRAM_ATTR kernel_trace_cycle_end(uint64_t now);

#endif

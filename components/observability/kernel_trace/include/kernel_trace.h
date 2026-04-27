#ifndef KERNEL_TRACE_H
#define KERNEL_TRACE_H

#include <stdint.h>

void kernel_trace_init(void);
void kernel_trace_cycle_start(uint64_t now);
void kernel_trace_cycle_end(uint64_t now);
void kernel_trace_process(void);

#endif

#ifndef RUNTIME_MONITOR_H
#define RUNTIME_MONITOR_H

#include <stdint.h>

void runtime_monitor_init(void);
void runtime_monitor_tick(void);
uint32_t runtime_monitor_get_last_cycle_us(void);
uint8_t runtime_monitor_is_fault(void);

#endif

#ifndef CONTROL_KERNEL_H
#define CONTROL_KERNEL_H

#include <stdint.h>

void control_kernel_init(void);
void control_kernel_task(void);

uint8_t  control_kernel_has_fault(void);
uint32_t control_kernel_get_last_cycle_us(void);

#endif

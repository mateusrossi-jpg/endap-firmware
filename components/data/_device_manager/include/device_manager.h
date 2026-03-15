#ifndef DEVICE_MANAGER_H
#define DEVICE_MANAGER_H

#include <stdint.h>
#include <stdbool.h>
//#include "registry.h"

uint8_t node_id;
uint8_t type;

void device_manager_init(void);

bool device_register(device_t *dev);

bool device_write(uint16_t id,int32_t value);

bool device_read(uint16_t id,int32_t *out);

void device_manager_process(void);

#endif

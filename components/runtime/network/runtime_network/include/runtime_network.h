#pragma once

#include <stdint.h>

void runtime_network_init(void);
void runtime_network_send(uint8_t *data, uint16_t len);

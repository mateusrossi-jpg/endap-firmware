#ifndef DETERMINISM_PROBE_H
#define DETERMINISM_PROBE_H

#include <stdint.h>
#include "esp_attr.h"

void IRAM_ATTR determinism_probe_cycle_start(uint64_t now);

void IRAM_ATTR determinism_probe_cycle_end(uint64_t now);

#endif

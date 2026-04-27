#ifndef DETERMINISM_PROBE_H
#define DETERMINISM_PROBE_H

#include <stdint.h>
#include "esp_attr.h"

void determinism_probe_init(void);
void determinism_probe_cycle_start(uint64_t now);
void determinism_probe_cycle_end(uint64_t now);
void determinism_probe_process(void);

#endif

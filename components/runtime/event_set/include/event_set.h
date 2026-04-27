#pragma once

#include <stdint.h>
#include <stdbool.h>

bool event_enqueue_set(uint16_t id, int32_t value);
void event_process_set_queue(void);

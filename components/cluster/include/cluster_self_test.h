#pragma once

#include <stdbool.h>
#include <stdint.h>

void cluster_self_test_init(uint32_t self_node_id);
bool cluster_self_test_available(void);
bool cluster_self_test_trigger(void);
bool cluster_self_test_is_running(void);
const char *cluster_self_test_phase(void);

#pragma once
#include <stdint.h>

#define DISCOVERY_PORT 5005
#define DISCOVERY_INTERVAL_MS 2000

void cluster_discovery_start(void);

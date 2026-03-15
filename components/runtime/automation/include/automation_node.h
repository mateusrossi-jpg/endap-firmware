#pragma once

#include "event_bus.h"

typedef struct automation_node
{
    uint16_t input;
    uint16_t output;

    int32_t threshold;

} automation_node_t;

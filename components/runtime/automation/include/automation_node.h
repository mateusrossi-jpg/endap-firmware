#pragma once

#include <stdint.h>

typedef struct automation_node
{
    uint16_t input;
    uint16_t output;
    int32_t threshold;
    uint8_t op;
    uint8_t mode;
    uint16_t duration_ms;
    int8_t on_true;
    int8_t on_false;
} automation_node_t;

#pragma once

#include <stdint.h>

#define AUTOMATION_MAX_INTERLOCK_COND 2

typedef struct automation_condition
{
    uint16_t channel;
    uint8_t op;
    int8_t target_val;
} automation_condition_t;

typedef struct automation_interlock
{
    uint8_t enabled;
    uint8_t logic_op;   /* 0 = AND, 1 = OR */
    uint8_t cond_count; /* 0..2 */
    uint8_t reserved;
    automation_condition_t conditions[AUTOMATION_MAX_INTERLOCK_COND];
} automation_interlock_t;

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

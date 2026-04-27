#pragma once
#include <stdint.h>
#include <stdbool.h>

typedef struct
{
    uint16_t id;
    int32_t value;
} io_cmd_t;

void io_command_init(void);
bool io_command_push(uint16_t id, int32_t value);
bool io_command_pop(io_cmd_t *cmd);


#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef struct
{
    bool armed;
    bool found;
    uint16_t input_id;
    const char *input_name;
    const char *input_description;
} input_learning_snapshot_t;

void input_learning_init(void);
void input_learning_arm(void);
void input_learning_cancel(void);
void input_learning_clear(void);
void input_learning_get_snapshot(input_learning_snapshot_t *out);

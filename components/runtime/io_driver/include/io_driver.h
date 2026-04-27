#pragma once

#include <stdint.h>

typedef struct
{
    uint16_t id;
    uint8_t level;
    uint32_t raw_edges;
    uint32_t stable_edges;
    uint32_t noise_edges;
    uint32_t recent_raw_edges;
    uint32_t recent_stable_edges;
    uint32_t recent_noise_edges;
} io_driver_input_diag_t;

void io_driver_init(void);
void io_driver_scan_inputs(void);
void io_driver_update(void);
void io_driver_process(void);
int io_driver_get_input_diag(io_driver_input_diag_t *out, int max_inputs);

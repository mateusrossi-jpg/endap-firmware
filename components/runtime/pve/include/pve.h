#pragma once

#include "pve_types.h"

#ifdef __cplusplus
extern "C" {
#endif

#define MAX_PVE_VARIABLES 7

extern pve_variable_t pve_variables[MAX_PVE_VARIABLES];

/**
 * @brief Initialize default PVE variable configurations (AIN1 and AIN2).
 */
void pve_init_variables(void);

const pve_variable_t *pve_get(uint32_t id);
uint32_t pve_count(void);

/**
 * @brief Scale a raw input value to a process value using fixed-point math.
 * 
 * @param raw Raw input value to scale
 * @param input_min Minimum expected raw value
 * @param input_max Maximum expected raw value
 * @param scaled_min Minimum target scaled value (fixed-point)
 * @param scaled_max Maximum target scaled value (fixed-point)
 * @return int32_t Scaled value in fixed-point representation
 */
int32_t pve_scale(
    int32_t raw,
    int32_t input_min,
    int32_t input_max,
    int32_t scaled_min,
    int32_t scaled_max
);

/**
 * @brief Update runtime PVE variable calculations with a new raw sample.
 * 
 * @param var Pointer to the PVE variable struct
 * @param raw_value The new raw value to register
 */
void pve_update(
    pve_variable_t *var,
    int32_t raw_value
);

int32_t pve_get_scaled_value(uint16_t state_id, int32_t fallback);

void pve_save_config(void);
void pve_load_config(void);


uint32_t pve_get_alarm_history(pve_alarm_event_t *out_buffer, uint32_t max_len);
void pve_clear_alarm_history(void);

#include "esp_err.h"
esp_err_t pve_i2c_scan(uint8_t *devices, int max_devices, int *out_count);

#ifdef __cplusplus
}
#endif


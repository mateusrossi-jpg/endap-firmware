#pragma once

#include <stdint.h>
#include <stdbool.h>

typedef enum {
    PVE_SOURCE_ADC_NATIVE = 0,
    PVE_SOURCE_ADC_EXTERNAL = 1,
    PVE_SOURCE_MODBUS = 2,
    PVE_SOURCE_ESPNOW = 3,
    PVE_SOURCE_VIRTUAL = 4
} pve_source_type_t;

typedef enum {
    PVE_ALARM_STATE_OK = 0,
    PVE_ALARM_STATE_HIGH = 1,
    PVE_ALARM_STATE_LOW = 2
} pve_alarm_state_t;

typedef struct {
    int32_t raw_value;
    int32_t scaled_value;
    uint8_t alarm_state;
} pve_runtime_t;

typedef struct {
    int32_t input_min;
    int32_t input_max;
    int32_t scaled_min;
    int32_t scaled_max;
    uint8_t decimals;
    char unit[8];
} pve_config_t;

typedef struct {
    int32_t high_limit;
    int32_t low_limit;
    int32_t hysteresis;
    uint8_t enabled;
} pve_alarm_t;

typedef struct {
    uint8_t source_type;
    uint32_t source_id;
    char name[16];
    pve_runtime_t runtime;
    pve_config_t config;
    pve_alarm_t alarm;
} pve_variable_t;

typedef struct {
    uint16_t state_id;
    uint16_t pve_id;
} pve_binding_t;

typedef struct {
    uint32_t timestamp;
    uint16_t pve_id;
    uint8_t state;
    int32_t scaled_value;
} pve_alarm_event_t;


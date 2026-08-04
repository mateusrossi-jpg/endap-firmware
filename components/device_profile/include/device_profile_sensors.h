#ifndef DEVICE_PROFILE_SENSORS_H
#define DEVICE_PROFILE_SENSORS_H

#include <stdint.h>
#include <stdbool.h>
#include "hal/gpio_types.h"

typedef struct {
    bool dht11_enabled;
    gpio_num_t dht11_gpio;
    bool ds18b20_enabled;
    gpio_num_t ds18b20_gpio;
    bool aht10_enabled;
    gpio_num_t aht10_sda_gpio;
    gpio_num_t aht10_scl_gpio;
} device_sensor_profile_t;

void device_profile_sensors_init(void);
const device_sensor_profile_t *device_profile_get_sensors(void);
bool device_profile_set_sensors(const device_sensor_profile_t *config);

#endif

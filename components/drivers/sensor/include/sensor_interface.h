#ifndef SENSOR_INTERFACE_H
#define SENSOR_INTERFACE_H

#include "esp_err.h"

typedef struct {
    esp_err_t (*init)(void);
    esp_err_t (*read)(float *temp, float *humi);
    esp_err_t (*deinit)(void);
    const char *name;
} sensor_driver_t;

#include "hal/gpio_types.h"

extern const sensor_driver_t dht11_driver;
extern const sensor_driver_t aht10_driver;
void dht11_set_gpio(gpio_num_t gpio);
#endif // SENSOR_INTERFACE_H

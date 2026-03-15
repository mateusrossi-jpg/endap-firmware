#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef enum
{
    HAL_GPIO_INPUT = 0,
    HAL_GPIO_OUTPUT
} hal_gpio_mode_t;

typedef enum
{
    HAL_GPIO_NOPULL = 0,
    HAL_GPIO_PULLUP,
    HAL_GPIO_PULLDOWN
} hal_gpio_pull_t;

typedef struct
{
    uint8_t pin;
    hal_gpio_mode_t mode;
    hal_gpio_pull_t pull;
} hal_gpio_config_t;

void hal_gpio_init(void);

bool hal_gpio_configure(const hal_gpio_config_t *cfg);

bool hal_gpio_write(uint8_t pin, bool level);

bool hal_gpio_read(uint8_t pin);

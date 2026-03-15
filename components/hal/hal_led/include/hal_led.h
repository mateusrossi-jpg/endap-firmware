#ifndef HAL_LED_H
#define HAL_LED_H

#include <stdint.h>

void hal_led_init(void);
void hal_led_set(uint8_t on);
void hal_led_toggle(void);

#endif

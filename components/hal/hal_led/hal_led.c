#include "hal_led.h"
#include "driver/gpio.h"

#define LED_GPIO GPIO_NUM_2

static uint8_t led_state = 0;

void hal_led_init(void)
{
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << LED_GPIO),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE
    };

    gpio_config(&io_conf);

    gpio_set_level(LED_GPIO, 0);
}

void hal_led_set(uint8_t on)
{
    led_state = on ? 1 : 0;
    gpio_set_level(LED_GPIO, led_state);
}

void hal_led_toggle(void)
{
    led_state = !led_state;
    gpio_set_level(LED_GPIO, led_state);
}

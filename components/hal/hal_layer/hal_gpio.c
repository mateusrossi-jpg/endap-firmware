#include "hal_gpio.h"
#include "driver/gpio.h"
#include "esp_log.h"

static const char *TAG = "HAL_GPIO";

void hal_gpio_init(void)
{
    ESP_LOGI(TAG, "HAL GPIO inicializado");
}

static gpio_mode_t convert_mode(hal_gpio_mode_t mode)
{
    switch (mode)
    {
        case HAL_GPIO_INPUT:  return GPIO_MODE_INPUT;
        case HAL_GPIO_OUTPUT: return GPIO_MODE_OUTPUT;
        default: return GPIO_MODE_DISABLE;
    }
}

static gpio_pullup_t pullup(hal_gpio_pull_t pull)
{
    return (pull == HAL_GPIO_PULLUP) ? GPIO_PULLUP_ENABLE : GPIO_PULLUP_DISABLE;
}

static gpio_pulldown_t pulldown(hal_gpio_pull_t pull)
{
    return (pull == HAL_GPIO_PULLDOWN) ? GPIO_PULLDOWN_ENABLE : GPIO_PULLDOWN_DISABLE;
}

bool hal_gpio_configure(const hal_gpio_config_t *cfg)
{
    if (!cfg)
        return false;

    if (cfg->pin >= GPIO_NUM_MAX)
        return false;

    gpio_config_t io = {
        .pin_bit_mask = 1ULL << cfg->pin,
        .mode = convert_mode(cfg->mode),
        .pull_up_en = pullup(cfg->pull),
        .pull_down_en = pulldown(cfg->pull),
        .intr_type = GPIO_INTR_DISABLE
    };

    esp_err_t err = gpio_config(&io);

    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "Erro config GPIO %d", cfg->pin);
        return false;
    }

    return true;
}

bool hal_gpio_write(uint8_t pin, bool level)
{
    if (pin >= GPIO_NUM_MAX)
        return false;

    gpio_set_level(pin, level);
    return true;
}

bool hal_gpio_read(uint8_t pin)
{
    if (pin >= GPIO_NUM_MAX)
        return false;

    return gpio_get_level(pin);
}

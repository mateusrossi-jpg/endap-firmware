#include "sensor_interface.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_rom_sys.h"

static const char *TAG = "SENSOR_DHT11";
static bool dht_initialized = false;
static gpio_num_t dht_gpio = GPIO_NUM_4;

void dht11_set_gpio(gpio_num_t gpio)
{
    dht_gpio = gpio;
}

#define DHT11_TIMEOUT 10000

static esp_err_t dht11_init(void)
{
    gpio_set_direction(dht_gpio, GPIO_MODE_INPUT_OUTPUT);
    gpio_set_pull_mode(dht_gpio, GPIO_PULLUP_ONLY);
    dht_initialized = true;
    ESP_LOGI(TAG, "DHT11 driver initialized on GPIO %d", dht_gpio);
    return ESP_OK;
}

static esp_err_t dht11_read(float *temp, float *humi)
{
    if (!dht_initialized) return ESP_FAIL;

    uint8_t data[5] = {0};
    
    // Start signal: Pull low > 18ms
    gpio_set_direction(dht_gpio, GPIO_MODE_OUTPUT);
    gpio_set_level(dht_gpio, 0);
    vTaskDelay(pdMS_TO_TICKS(20));
    gpio_set_level(dht_gpio, 1);
    esp_rom_delay_us(30);
    gpio_set_direction(dht_gpio, GPIO_MODE_INPUT);

    static portMUX_TYPE dht_mux = portMUX_INITIALIZER_UNLOCKED;
    portENTER_CRITICAL(&dht_mux);

    // Wait for response
    uint32_t timeout = DHT11_TIMEOUT;
    while(gpio_get_level(dht_gpio) == 1 && timeout--) esp_rom_delay_us(1);
    if (timeout == 0) { portEXIT_CRITICAL(&dht_mux); return ESP_ERR_TIMEOUT; }
    
    timeout = DHT11_TIMEOUT;
    while(gpio_get_level(dht_gpio) == 0 && timeout--) esp_rom_delay_us(1); // sensor pulls low 80us
    if (timeout == 0) { portEXIT_CRITICAL(&dht_mux); return ESP_ERR_TIMEOUT; }
    
    timeout = DHT11_TIMEOUT;
    while(gpio_get_level(dht_gpio) == 1 && timeout--) esp_rom_delay_us(1); // sensor pulls high 80us
    if (timeout == 0) { portEXIT_CRITICAL(&dht_mux); return ESP_ERR_TIMEOUT; }

    // Read bits
    for (int i = 0; i < 40; i++) {
        timeout = DHT11_TIMEOUT;
        while(gpio_get_level(dht_gpio) == 0 && timeout--) esp_rom_delay_us(1); // Wait for high (bit start)
        
        esp_rom_delay_us(40); // Wait 40us to check level
        if (gpio_get_level(dht_gpio)) {
            data[i/8] |= (1 << (7 - (i%8)));
            timeout = DHT11_TIMEOUT;
            while(gpio_get_level(dht_gpio) == 1 && timeout--) esp_rom_delay_us(1); // Wait for low (bit end)
        }
    }
    
    portEXIT_CRITICAL(&dht_mux);

    // Verify checksum
    if (data[4] != (data[0] + data[1] + data[2] + data[3])) {
        ESP_LOGE(TAG, "Checksum error");
        return ESP_ERR_INVALID_CRC;
    }

    *humi = (float)data[0] + (float)data[1] * 0.1f;
    *temp = (float)data[2] + (float)data[3] * 0.1f;
    
    ESP_LOGD(TAG, "DHT11 read: T=%.1fC, H=%.1f%%", *temp, *humi);
    return ESP_OK;
}

static esp_err_t dht11_deinit(void)
{
    dht_initialized = false;
    return ESP_OK;
}

const sensor_driver_t dht11_driver = {
    .init = dht11_init,
    .read = dht11_read,
    .deinit = dht11_deinit,
    .name = "DHT11"
};

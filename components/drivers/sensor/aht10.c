#include "sensor_interface.h"
#include "driver/i2c.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "SENSOR_AHT10";
static bool aht_initialized = false;

// I2C Config (Assuming these match the system-wide I2C setup)
#define I2C_MASTER_NUM I2C_NUM_0
#define AHT10_ADDR 0x38

static uint8_t active_aht_addr = 0x38;

static esp_err_t aht10_init(void)
{
    vTaskDelay(pdMS_TO_TICKS(40));

    // Soft reset
    uint8_t reset_cmd[] = { 0xBA };
    uint8_t addrs[2] = { 0x38, 0x39 };
    bool found = false;

    for (int i = 0; i < 2; i++) {
        uint8_t addr = addrs[i];
        esp_err_t err = i2c_master_write_to_device(I2C_MASTER_NUM, addr, reset_cmd, sizeof(reset_cmd), pdMS_TO_TICKS(50));
        if (err == ESP_OK) {
            active_aht_addr = addr;
            found = true;
            break;
        }
    }

    if (!found) {
        ESP_LOGW(TAG, "AHT10/20 not responding on address 0x38 or 0x39");
        aht_initialized = false;
        return ESP_FAIL;
    }

    vTaskDelay(pdMS_TO_TICKS(20));

    // Calibration/Init command
    uint8_t init_cmd[] = { 0xE1, 0x08, 0x00 };
    esp_err_t err = i2c_master_write_to_device(I2C_MASTER_NUM, active_aht_addr, init_cmd, sizeof(init_cmd), pdMS_TO_TICKS(100));
    if (err == ESP_OK) {
        aht_initialized = true;
        ESP_LOGI(TAG, "AHT10/20 initialized successfully at address 0x%02X", active_aht_addr);
    } else {
        ESP_LOGW(TAG, "AHT10/20 calibration cmd failed on 0x%02X", active_aht_addr);
        aht_initialized = false;
    }
    return err;
}

static esp_err_t aht10_read(float *temp, float *humi)
{
    if (!aht_initialized) {
        if (aht10_init() != ESP_OK) {
            return ESP_FAIL;
        }
    }

    uint8_t trigger_cmd[] = { 0xAC, 0x33, 0x00 };
    esp_err_t err = i2c_master_write_to_device(I2C_MASTER_NUM, active_aht_addr, trigger_cmd, sizeof(trigger_cmd), pdMS_TO_TICKS(100));
    if (err != ESP_OK) {
        aht_initialized = false;
        return err;
    }

    vTaskDelay(pdMS_TO_TICKS(80));

    uint8_t data[6] = {0};
    err = i2c_master_read_from_device(I2C_MASTER_NUM, active_aht_addr, data, sizeof(data), pdMS_TO_TICKS(100));
    if (err != ESP_OK) {
        aht_initialized = false;
        return err;
    }

    uint32_t raw_hum = ((uint32_t)data[1] << 12) | ((uint32_t)data[2] << 4) | (data[3] >> 4);
    uint32_t raw_temp = (((uint32_t)data[3] & 0x0F) << 16) | ((uint32_t)data[4] << 8) | data[5];

    *humi = (float)(raw_hum * 100.0 / 1048576.0);
    *temp = (float)(raw_temp * 200.0 / 1048576.0 - 50.0);

    return ESP_OK;
}

static esp_err_t aht10_deinit(void)
{
    aht_initialized = false;
    return ESP_OK;
}

const sensor_driver_t aht10_driver = {
    .init = aht10_init,
    .read = aht10_read,
    .deinit = aht10_deinit,
    .name = "AHT10"
};

#include "device_profile_sensors.h"
#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"
#include <string.h>

static const char *TAG = "SENSOR_PROFILE";
#define SENSOR_PROFILE_NAMESPACE "endap_sensors"
#define SENSOR_PROFILE_KEY "config_v1"

#define SENSOR_PROFILE_MAGIC 0x5E455301
#define SENSOR_PROFILE_VERSION 1

typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint16_t version;
    uint8_t dht11_enabled;
    int8_t dht11_gpio;
    uint8_t ds18b20_enabled;
    int8_t ds18b20_gpio;
    uint8_t aht10_enabled;
    int8_t aht10_sda_gpio;
    int8_t aht10_scl_gpio;
    uint32_t crc;
} sensor_profile_blob_v1_t;

static device_sensor_profile_t current_sensors = {
    .dht11_enabled = false,
    .dht11_gpio = -1,
    .ds18b20_enabled = false,
    .ds18b20_gpio = -1,
    .aht10_enabled = false,
    .aht10_sda_gpio = GPIO_NUM_21,
    .aht10_scl_gpio = GPIO_NUM_22,
};

static uint32_t sensor_profile_crc32(const void *data, size_t length)
{
    const uint8_t *p = (const uint8_t *)data;
    uint32_t crc = 0xFFFFFFFF;
    for (size_t i = 0; i < length; i++)
    {
        crc ^= p[i];
        for (int j = 0; j < 8; j++)
        {
            if (crc & 1)
                crc = (crc >> 1) ^ 0xEDB88320;
            else
                crc >>= 1;
        }
    }
    return crc ^ 0xFFFFFFFF;
}

void device_profile_sensors_init(void)
{
    nvs_handle_t nvs;
    sensor_profile_blob_v1_t blob;
    size_t len = sizeof(blob);

    if (nvs_open(SENSOR_PROFILE_NAMESPACE, NVS_READONLY, &nvs) != ESP_OK)
        return;

    if (nvs_get_blob(nvs, SENSOR_PROFILE_KEY, &blob, &len) == ESP_OK && len == sizeof(blob))
    {
        uint32_t crc = sensor_profile_crc32(&blob, sizeof(blob) - sizeof(blob.crc));
        if (blob.magic == SENSOR_PROFILE_MAGIC && blob.version == SENSOR_PROFILE_VERSION && blob.crc == crc)
        {
            current_sensors.dht11_enabled = (blob.dht11_enabled != 0);
            current_sensors.dht11_gpio = (gpio_num_t)blob.dht11_gpio;
            current_sensors.ds18b20_enabled = (blob.ds18b20_enabled != 0);
            current_sensors.ds18b20_gpio = (gpio_num_t)blob.ds18b20_gpio;
            current_sensors.aht10_enabled = (blob.aht10_enabled != 0);
            current_sensors.aht10_sda_gpio = (gpio_num_t)blob.aht10_sda_gpio;
            current_sensors.aht10_scl_gpio = (gpio_num_t)blob.aht10_scl_gpio;
            ESP_LOGI(TAG, "Sensor profile loaded.");
        }
        else
        {
            ESP_LOGW(TAG, "Invalid sensor profile blob");
        }
    }
    
    nvs_close(nvs);
}

const device_sensor_profile_t *device_profile_get_sensors(void)
{
    return &current_sensors;
}

bool device_profile_set_sensors(const device_sensor_profile_t *config)
{
    if (!config) return false;
    
    nvs_handle_t nvs;
    if (nvs_open(SENSOR_PROFILE_NAMESPACE, NVS_READWRITE, &nvs) != ESP_OK)
        return false;

    sensor_profile_blob_v1_t blob;
    memset(&blob, 0, sizeof(blob));
    blob.magic = SENSOR_PROFILE_MAGIC;
    blob.version = SENSOR_PROFILE_VERSION;
    blob.dht11_enabled = config->dht11_enabled ? 1 : 0;
    blob.dht11_gpio = (int8_t)config->dht11_gpio;
    blob.ds18b20_enabled = config->ds18b20_enabled ? 1 : 0;
    blob.ds18b20_gpio = (int8_t)config->ds18b20_gpio;
    blob.aht10_enabled = config->aht10_enabled ? 1 : 0;
    blob.aht10_sda_gpio = (int8_t)config->aht10_sda_gpio;
    blob.aht10_scl_gpio = (int8_t)config->aht10_scl_gpio;
    blob.crc = sensor_profile_crc32(&blob, sizeof(blob) - sizeof(blob.crc));

    if (nvs_set_blob(nvs, SENSOR_PROFILE_KEY, &blob, sizeof(blob)) != ESP_OK)
    {
        nvs_close(nvs);
        return false;
    }

    nvs_commit(nvs);
    nvs_close(nvs);

    current_sensors = *config;
    return true;
}

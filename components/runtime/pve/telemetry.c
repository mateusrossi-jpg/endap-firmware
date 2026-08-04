#include "telemetry.h"
#include "state.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_random.h"
#include "driver/i2c.h"
#include "esp_rom_sys.h"
#include "sensor_interface.h"
#include "device_profile_sensors.h"
#include "pve.h"

static const char *TAG = "TELEMETRY";

// AHT10 (I2C) - Keep as is for now if needed, but DHT11 and DS18B20 are dynamic
#define I2C_MASTER_SDA_IO 21
#define I2C_MASTER_SCL_IO 22
#define I2C_MASTER_NUM I2C_NUM_0
#define I2C_MASTER_FREQ_HZ 100000

// DS18B20 Dynamic
static bool ds_present = false;
static int ds_gpio = -1;

static void onewire_pin_mode(gpio_mode_t mode)
{
    if (ds_gpio < 0) return;
    gpio_set_direction((gpio_num_t)ds_gpio, mode);
}

static bool onewire_reset(void)
{
    if (ds_gpio < 0) return false;
    bool presence = false;
    onewire_pin_mode(GPIO_MODE_OUTPUT);
    gpio_set_level((gpio_num_t)ds_gpio, 0);
    esp_rom_delay_us(480);
    onewire_pin_mode(GPIO_MODE_INPUT);
    esp_rom_delay_us(70);
    presence = (gpio_get_level((gpio_num_t)ds_gpio) == 0);
    esp_rom_delay_us(410);
    return presence;
}

static void onewire_write_bit(bool bit)
{
    if (ds_gpio < 0) return;
    onewire_pin_mode(GPIO_MODE_OUTPUT);
    gpio_set_level((gpio_num_t)ds_gpio, 0);
    if (bit) {
        esp_rom_delay_us(6);
        onewire_pin_mode(GPIO_MODE_INPUT);
        esp_rom_delay_us(64);
    } else {
        esp_rom_delay_us(60);
        onewire_pin_mode(GPIO_MODE_INPUT);
        esp_rom_delay_us(10);
    }
}

static bool onewire_read_bit(void)
{
    if (ds_gpio < 0) return false;
    bool bit = false;
    onewire_pin_mode(GPIO_MODE_OUTPUT);
    gpio_set_level((gpio_num_t)ds_gpio, 0);
    esp_rom_delay_us(6);
    onewire_pin_mode(GPIO_MODE_INPUT);
    esp_rom_delay_us(9);
    bit = (gpio_get_level((gpio_num_t)ds_gpio) != 0);
    esp_rom_delay_us(55);
    return bit;
}

static void onewire_write_byte(uint8_t val)
{
    for (int i = 0; i < 8; i++) {
        onewire_write_bit(val & (1 << i));
    }
}

static uint8_t onewire_read_byte(void)
{
    uint8_t val = 0;
    for (int i = 0; i < 8; i++) {
        if (onewire_read_bit()) {
            val |= (1 << i);
        }
    }
    return val;
}

static void ds18b20_init_hw(int gpio)
{
    ds_gpio = gpio;
    if (ds_gpio < 0) return;
    gpio_config_t io_conf = {
        .intr_type = GPIO_INTR_DISABLE,
        .mode = GPIO_MODE_INPUT,
        .pin_bit_mask = (1ULL << ds_gpio),
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .pull_up_en = GPIO_PULLUP_ENABLE
    };
    gpio_config(&io_conf);

    ds_present = onewire_reset();
    if (ds_present) {
        ESP_LOGI(TAG, "Sensor DS18B20 detectado no pino GPIO %d", ds_gpio);
    } else {
        ESP_LOGW(TAG, "DS18B20 nao detectado no pino GPIO %d - Usando fallback simulado para demonstracao", ds_gpio);
    }
}

static bool ds18b20_read_hw(int32_t *out_temp_x10)
{
    if (ds_gpio < 0) return false;
    if (!onewire_reset()) {
        ds_present = false;
        return false;
    }
    ds_present = true;

    onewire_write_byte(0xCC); // Skip ROM
    onewire_write_byte(0x44); // Convert T

    vTaskDelay(pdMS_TO_TICKS(750));

    if (!onewire_reset()) return false;

    onewire_write_byte(0xCC); // Skip ROM
    onewire_write_byte(0xBE); // Read Scratchpad

    uint8_t lsb = onewire_read_byte();
    uint8_t msb = onewire_read_byte();

    onewire_reset();

    int16_t raw_temp = (msb << 8) | lsb;
    double temp = (double)raw_temp * 0.0625;

    *out_temp_x10 = (int32_t)(temp * 10.0);
    return true;
}

static bool i2c_bus_initialized = false;

static void init_i2c_bus(gpio_num_t sda, gpio_num_t scl)
{
    if (sda < 0 || scl < 0) return;
    if (i2c_bus_initialized) return;

    i2c_config_t conf = {
        .mode = I2C_MODE_MASTER,
        .sda_io_num = sda,
        .sda_pullup_en = GPIO_PULLUP_ENABLE,
        .scl_io_num = scl,
        .scl_pullup_en = GPIO_PULLUP_ENABLE,
        .master.clk_speed = I2C_MASTER_FREQ_HZ,
    };
    i2c_param_config(I2C_MASTER_NUM, &conf);
    i2c_driver_install(I2C_MASTER_NUM, conf.mode, 0, 0, 0);
    i2c_bus_initialized = true;
    ESP_LOGI(TAG, "I2C bus initialized on SDA=%d, SCL=%d", sda, scl);
}

static bool aht10_inited = false;
static bool dht11_inited = false;
static bool ds18b20_inited = false;
static gpio_num_t last_aht_sda = (gpio_num_t)-1;
static gpio_num_t last_aht_scl = (gpio_num_t)-1;
static gpio_num_t last_dht_gpio = (gpio_num_t)-1;
static gpio_num_t last_ds_gpio = (gpio_num_t)-1;

static void telemetry_task(void *arg)
{
    (void)arg;

    double sim_ds_temp = 37.8;
    double sim_aht_temp = 24.5;
    double sim_aht_hum = 55.2;
    double sim_dht_temp = 23.8;
    double sim_dht_hum = 48.5;

    while (1)
    {
        const device_sensor_profile_t *sensors = device_profile_get_sensors();
        int32_t dht_temp_val = 0;
        int32_t dht_hum_val = 0;
        int32_t ds_temp_val = 0;
        int32_t aht_temp_val = 0;
        int32_t aht_hum_val = 0;
        float temp, humi;

        if (sensors && sensors->aht10_enabled)
        {
            if (!aht10_inited || last_aht_sda != sensors->aht10_sda_gpio || last_aht_scl != sensors->aht10_scl_gpio)
            {
                i2c_bus_initialized = false;
                init_i2c_bus(sensors->aht10_sda_gpio, sensors->aht10_scl_gpio);
                if (aht10_driver.init() == ESP_OK)
                {
                    aht10_inited = true;
                    last_aht_sda = sensors->aht10_sda_gpio;
                    last_aht_scl = sensors->aht10_scl_gpio;
                    ESP_LOGI(TAG, "AHT10 sensor driver initialized dynamically");
                }
            }

            if (aht10_driver.read(&temp, &humi) == ESP_OK)
            {
                aht_temp_val = (int32_t)(temp * 10.0);
                aht_hum_val = (int32_t)(humi * 10.0);
            }
            else
            {
                // Fallback simulation reading if hardware is absent/error
                double noise_t = ((double)(esp_random() % 100) - 50.0) / 300.0;
                double noise_h = ((double)(esp_random() % 100) - 50.0) / 200.0;
                sim_aht_temp += noise_t;
                sim_aht_hum += noise_h;
                if (sim_aht_temp < 15.0) sim_aht_temp = 15.0;
                if (sim_aht_temp > 40.0) sim_aht_temp = 40.0;
                if (sim_aht_hum < 20.0) sim_aht_hum = 20.0;
                if (sim_aht_hum > 90.0) sim_aht_hum = 90.0;

                aht_temp_val = (int32_t)(sim_aht_temp * 10.0);
                aht_hum_val = (int32_t)(sim_aht_hum * 10.0);
            }

            state_set_int(14, aht_temp_val);
            state_set_int(15, aht_hum_val);
            pve_update(&pve_variables[2], aht_temp_val);
            pve_update(&pve_variables[3], aht_hum_val);
        }
        else
        {
            aht10_inited = false;
        }

        if (sensors && sensors->dht11_enabled)
        {
            if (!dht11_inited || last_dht_gpio != sensors->dht11_gpio)
            {
                dht11_set_gpio(sensors->dht11_gpio);
                if (dht11_driver.init() == ESP_OK)
                {
                    dht11_inited = true;
                    last_dht_gpio = sensors->dht11_gpio;
                    ESP_LOGI(TAG, "DHT11 sensor driver initialized dynamically on GPIO %d", sensors->dht11_gpio);
                }
            }

            if (dht11_driver.read(&temp, &humi) == ESP_OK)
            {
                dht_temp_val = (int32_t)(temp * 10.0);
                dht_hum_val = (int32_t)(humi * 10.0);
            }
            else
            {
                // Fallback simulation reading if hardware is absent/error
                double noise_t = ((double)(esp_random() % 100) - 50.0) / 300.0;
                double noise_h = ((double)(esp_random() % 100) - 50.0) / 200.0;
                sim_dht_temp += noise_t;
                sim_dht_hum += noise_h;
                if (sim_dht_temp < 15.0) sim_dht_temp = 15.0;
                if (sim_dht_temp > 40.0) sim_dht_temp = 40.0;
                if (sim_dht_hum < 20.0) sim_dht_hum = 20.0;
                if (sim_dht_hum > 90.0) sim_dht_hum = 90.0;

                dht_temp_val = (int32_t)(sim_dht_temp * 10.0);
                dht_hum_val = (int32_t)(sim_dht_hum * 10.0);
            }

            state_set_int(17, dht_temp_val);
            state_set_int(18, dht_hum_val);
            pve_update(&pve_variables[5], dht_temp_val);
            pve_update(&pve_variables[6], dht_hum_val);
        }
        else
        {
            dht11_inited = false;
        }

        if (sensors && sensors->ds18b20_enabled)
        {
            if (!ds18b20_inited || last_ds_gpio != sensors->ds18b20_gpio)
            {
                ds18b20_init_hw(sensors->ds18b20_gpio);
                ds18b20_inited = true;
                last_ds_gpio = sensors->ds18b20_gpio;
                ESP_LOGI(TAG, "DS18B20 sensor driver initialized dynamically on GPIO %d", sensors->ds18b20_gpio);
            }

            if (!ds18b20_read_hw(&ds_temp_val))
            {
                double noise_ds = ((double)(esp_random() % 100) - 50.0) / 200.0;
                sim_ds_temp += noise_ds;

                if (sim_ds_temp < 35.0) sim_ds_temp = 35.0;
                if (sim_ds_temp > 45.0) sim_ds_temp = 45.0;

                ds_temp_val = (int32_t)(sim_ds_temp * 10.0);
            }
            state_set_int(16, ds_temp_val);
            pve_update(&pve_variables[4], ds_temp_val);
        }
        else
        {
            ds18b20_inited = false;
        }

        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

void telemetry_init(void)
{
    xTaskCreatePinnedToCore(
        telemetry_task,
        "telemetry",
        3078,
        NULL,
        1,
        NULL,
        0
    );
    ESP_LOGI(TAG, "Telemetry background task started successfully");
}

esp_err_t pve_i2c_scan(uint8_t *devices, int max_devices, int *out_count)
{
    if (!devices || !out_count) {
        return ESP_ERR_INVALID_ARG;
    }
    const device_sensor_profile_t *sensors = device_profile_get_sensors();
    gpio_num_t sda = (sensors && sensors->aht10_enabled) ? sensors->aht10_sda_gpio : GPIO_NUM_21;
    gpio_num_t scl = (sensors && sensors->aht10_enabled) ? sensors->aht10_scl_gpio : GPIO_NUM_22;
    init_i2c_bus(sda, scl);

    int count = 0;
    for (uint8_t addr = 0x03; addr < 0x78; addr++) {
        esp_err_t err = i2c_master_write_to_device(I2C_MASTER_NUM, addr, NULL, 0, pdMS_TO_TICKS(50));
        if (err == ESP_OK) {
            if (count < max_devices) {
                devices[count++] = addr;
            }
        }
    }
    *out_count = count;
    return ESP_OK;
}

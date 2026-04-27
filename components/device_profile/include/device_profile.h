#pragma once

#include "driver/gpio.h"

#include <stdbool.h>
#include <stdint.h>

typedef struct
{
    uint16_t id;
    const char *name;
    const char *description;
    gpio_num_t gpio;
    bool active_low;
    uint8_t debounce_samples;
} device_input_profile_t;

typedef struct
{
    uint16_t id;
    const char *name;
    const char *description;
    gpio_num_t gpio;
    bool active_low;
} device_output_profile_t;

typedef struct
{
    uint16_t input_id;
    uint16_t output_id;
    int32_t threshold;
} device_default_automation_t;

typedef struct
{
    gpio_num_t gpio;
    const char *label;
} device_gpio_option_t;

typedef struct
{
    int spi_host_id;
    gpio_num_t mosi_gpio;
    gpio_num_t miso_gpio;
    gpio_num_t sclk_gpio;
    gpio_num_t cs_gpio;
    gpio_num_t int_gpio;
    gpio_num_t reset_gpio;
    int phy_addr;
    uint8_t clock_mhz;
} device_network_w5500_profile_t;

typedef enum
{
    DEVICE_PROFILE_ETH_NONE = 0,
    DEVICE_PROFILE_ETH_INTERNAL_LAN8720 = 1,
    DEVICE_PROFILE_ETH_SPI_W5500 = 2,
} device_profile_ethernet_mode_t;

typedef enum
{
    DEVICE_PROFILE_TRANSPORT_NONE = 0,
    DEVICE_PROFILE_TRANSPORT_WIFI = 1,
    DEVICE_PROFILE_TRANSPORT_ETHERNET = 2,
    DEVICE_PROFILE_TRANSPORT_RS485 = 3,
} device_profile_transport_t;

typedef enum
{
    DEVICE_PROFILE_NETWORK_CONFIG_OK = 0,
    DEVICE_PROFILE_NETWORK_CONFIG_UNSUPPORTED = 1,
    DEVICE_PROFILE_NETWORK_CONFIG_PERSIST_FAILED = 2,
} device_profile_network_config_result_t;

typedef enum
{
    DEVICE_CHANNEL_CLASS_DIGITAL_INPUT = 0,
    DEVICE_CHANNEL_CLASS_DIGITAL_OUTPUT = 1,
    DEVICE_CHANNEL_CLASS_ANALOG_INPUT = 2,
    DEVICE_CHANNEL_CLASS_ANALOG_OUTPUT = 3,
} device_channel_class_t;

typedef enum
{
    DEVICE_CHANNEL_BACKEND_GPIO = 0,
    DEVICE_CHANNEL_BACKEND_MCP23X17 = 1,
    DEVICE_CHANNEL_BACKEND_ADC_NATIVE = 2,
    DEVICE_CHANNEL_BACKEND_ADC_EXTERNAL = 3,
} device_channel_backend_t;

typedef struct
{
    const char *group_id;
    const char *label;
    device_channel_class_t channel_class;
    device_channel_backend_t backend;
    uint16_t nominal_capacity;
    uint16_t default_slots;
    bool implemented_now;
    bool dashboard_ready;
    bool expansion_path;
    const char *addressing_model;
    const char *notes;
} device_channel_inventory_group_t;

typedef struct
{
    bool supports_mcp23x17;
    uint8_t recommended_mcp_instances;
    uint8_t channels_per_mcp;
    bool supports_ads1115;
    uint8_t recommended_external_adc_instances;
    uint8_t channels_per_external_adc;
    uint8_t native_analog_input_channels;
    const char *notes;
} device_expansion_capabilities_t;

typedef struct
{
    uint16_t local_input_slot_capacity;
    uint16_t local_output_slot_capacity;
    uint16_t default_input_count;
    uint16_t default_output_count;
    bool distributed_scaling;
    bool supports_remote_nodes;
    bool supports_mcp_digital;
    bool supports_native_analog;
    bool supports_external_analog;
    const char *global_capacity_mode;
    const char *local_capacity_mode;
    const char *recommended_scaling_path;
} device_node_capabilities_t;

typedef struct
{
    bool wifi_supported;
    bool ethernet_supported;
    bool rs485_supported;
    bool wifi_enabled;
    bool ethernet_enabled;
    bool rs485_enabled;
    bool onboarding_pending;
    device_profile_transport_t primary_transport;
    device_profile_transport_t fallback_transport;
    uint32_t failover_delay_ms;
    uint32_t recovery_hysteresis_ms;
    device_profile_ethernet_mode_t ethernet_mode;
    const char *label;
    device_network_w5500_profile_t w5500;
} device_network_profile_t;

int device_profile_input_count(void);
int device_profile_input_capacity(void);
const device_input_profile_t *device_profile_input_at(int index);
uint16_t device_profile_input_id_at(int index);
const device_input_profile_t *device_profile_find_input(uint16_t id);
bool device_profile_is_valid_input(uint16_t id);
bool device_profile_is_default_input_id(uint16_t id);
bool device_profile_is_extra_input_id(uint16_t id);
int device_profile_input_gpio_option_count(void);
const device_gpio_option_t *device_profile_input_gpio_option_at(int index);
bool device_profile_input_gpio_allowed(gpio_num_t gpio);

int device_profile_output_count(void);
int device_profile_output_capacity(void);
const device_output_profile_t *device_profile_output_at(int index);
uint16_t device_profile_output_id_at(int index);
const device_output_profile_t *device_profile_find_output(uint16_t id);
bool device_profile_is_valid_output(uint16_t id);
bool device_profile_is_default_output_id(uint16_t id);
bool device_profile_is_extra_output_id(uint16_t id);
int device_profile_output_gpio_option_count(void);
const device_gpio_option_t *device_profile_output_gpio_option_at(int index);
bool device_profile_output_gpio_allowed(gpio_num_t gpio);
uint16_t device_profile_preferred_self_test_output_id(void);

const device_node_capabilities_t *device_profile_node_capabilities(void);

const device_expansion_capabilities_t *device_profile_expansion_capabilities(void);
int device_profile_channel_inventory_group_count(void);
const device_channel_inventory_group_t *device_profile_channel_inventory_group_at(int index);

const device_network_profile_t *device_profile_network(void);
const device_network_w5500_profile_t *device_profile_w5500(void);
bool device_profile_w5500_is_configured(void);
bool device_profile_supports_wifi(void);
bool device_profile_supports_ethernet(void);
bool device_profile_supports_rs485(void);
bool device_profile_transport_supported(device_profile_transport_t transport);
bool device_profile_transport_enabled(device_profile_transport_t transport);
bool device_profile_transport_visible(device_profile_transport_t transport);

bool device_profile_network_onboarding_pending(void);
bool device_profile_should_start_wifi_on_boot(void);
bool device_profile_should_start_ethernet_on_boot(void);
bool device_profile_should_start_rs485_on_boot(void);
device_profile_transport_t device_profile_primary_transport(void);
device_profile_transport_t device_profile_fallback_transport(void);
uint32_t device_profile_failover_delay_ms(void);
uint32_t device_profile_recovery_hysteresis_ms(void);

device_profile_network_config_result_t device_profile_set_network_enabled(bool wifi_enabled,
                                                                          bool ethernet_enabled,
                                                                          bool rs485_enabled);

device_profile_network_config_result_t device_profile_set_transport_policy(bool onboarding_pending,
                                                                          device_profile_transport_t primary_transport,
                                                                          device_profile_transport_t fallback_transport,
                                                                          uint32_t failover_delay_ms,
                                                                          uint32_t recovery_hysteresis_ms);

bool device_profile_gpio_is_input_only(gpio_num_t gpio);

int device_profile_copy_local_io_ids(uint16_t *out_ids, int max_ids);

int device_profile_default_input_count(void);
int device_profile_default_output_count(void);

int device_profile_default_automation_count(void);
const device_default_automation_t *device_profile_default_automation_at(int index);

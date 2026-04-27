#include "device_profile.h"

#include "io_map.h"
#include "hal/spi_types.h"
#include "sdkconfig.h"
#include "esp_log.h"
#include "nvs.h"

#include <inttypes.h>
#include <stddef.h>
#include <string.h>

#define TAG "DEV_PROFILE"
#define ARRAY_LEN(x) ((int)(sizeof(x) / sizeof((x)[0])))
#define DEVICE_PROFILE_NETWORK_NAMESPACE "dev_profile"
#define DEVICE_PROFILE_NETWORK_KEY "net_cfg_v2"
#define DEVICE_PROFILE_NETWORK_MAGIC 0x454E4450U
#define DEVICE_PROFILE_NETWORK_VERSION 3U
#define DEVICE_PROFILE_FAILOVER_DELAY_DEFAULT_MS 5000U
#define DEVICE_PROFILE_RECOVERY_HYST_DEFAULT_MS 15000U

#ifndef CONFIG_ENDAP_WIFI_ENABLED
#define CONFIG_ENDAP_WIFI_ENABLED 1
#endif

#ifndef CONFIG_ENDAP_ETHERNET_ENABLED
#define CONFIG_ENDAP_ETHERNET_ENABLED 0
#endif

#ifndef CONFIG_ENDAP_W5500_SPI_HOST
#define CONFIG_ENDAP_W5500_SPI_HOST SPI2_HOST
#endif

#ifndef CONFIG_ENDAP_W5500_CLOCK_MHZ
#define CONFIG_ENDAP_W5500_CLOCK_MHZ 20
#endif

#ifndef CONFIG_ENDAP_W5500_PHY_ADDR
#define CONFIG_ENDAP_W5500_PHY_ADDR 1
#endif

#ifndef CONFIG_ENDAP_W5500_MOSI_GPIO
#define CONFIG_ENDAP_W5500_MOSI_GPIO -1
#endif

#ifndef CONFIG_ENDAP_W5500_MISO_GPIO
#define CONFIG_ENDAP_W5500_MISO_GPIO -1
#endif

#ifndef CONFIG_ENDAP_W5500_SCLK_GPIO
#define CONFIG_ENDAP_W5500_SCLK_GPIO -1
#endif

#ifndef CONFIG_ENDAP_W5500_CS_GPIO
#define CONFIG_ENDAP_W5500_CS_GPIO -1
#endif

#ifndef CONFIG_ENDAP_W5500_INT_GPIO
#define CONFIG_ENDAP_W5500_INT_GPIO -1
#endif

#ifndef CONFIG_ENDAP_W5500_RESET_GPIO
#define CONFIG_ENDAP_W5500_RESET_GPIO -1
#endif

static const device_input_profile_t input_profile[] =
{
    {ENDAP_INPUT_ID(0), "Input 10", "GPIO 18 • active-low • debounced", GPIO_NUM_18, true, 5},
    {ENDAP_INPUT_ID(1), "Input 11", "GPIO 19 • active-low • debounced", GPIO_NUM_19, true, 5},
    {ENDAP_INPUT_ID(2), "Input 12", "Canal adicional • escolha um GPIO pela dashboard", GPIO_NUM_NC, false, 5},
    {ENDAP_INPUT_ID(3), "Input 13", "Canal adicional • escolha um GPIO pela dashboard", GPIO_NUM_NC, false, 5},
    {ENDAP_INPUT_ID(4), "Input 14", "Canal adicional • escolha um GPIO pela dashboard", GPIO_NUM_NC, false, 5},
    {ENDAP_INPUT_ID(5), "Input 15", "Canal adicional • escolha um GPIO pela dashboard", GPIO_NUM_NC, false, 5},
    {ENDAP_INPUT_ID(6), "Input 16", "Canal adicional • escolha um GPIO pela dashboard", GPIO_NUM_NC, false, 5},
    {ENDAP_INPUT_ID(7), "Input 17", "Canal adicional • escolha um GPIO pela dashboard", GPIO_NUM_NC, false, 5},
    {ENDAP_INPUT_ID(8), "Input 18", "Canal adicional • escolha um GPIO pela dashboard", GPIO_NUM_NC, false, 5},
    {ENDAP_INPUT_ID(9), "Input 19", "Canal adicional • escolha um GPIO pela dashboard", GPIO_NUM_NC, false, 5},
    {ENDAP_INPUT_ID(10), "Input 20", "Canal adicional • escolha um GPIO pela dashboard", GPIO_NUM_NC, false, 5},
    {ENDAP_INPUT_ID(11), "Input 21", "Canal adicional • escolha um GPIO pela dashboard", GPIO_NUM_NC, false, 5},
    {ENDAP_INPUT_ID(12), "Input 22", "Canal adicional • escolha um GPIO pela dashboard", GPIO_NUM_NC, false, 5},
    {ENDAP_INPUT_ID(13), "Input 23", "Canal adicional • escolha um GPIO pela dashboard", GPIO_NUM_NC, false, 5},
    {ENDAP_INPUT_ID(14), "Input 24", "Canal adicional • escolha um GPIO pela dashboard", GPIO_NUM_NC, false, 5},
    {ENDAP_INPUT_ID(15), "Input 25", "Canal adicional • escolha um GPIO pela dashboard", GPIO_NUM_NC, false, 5},
};

static const device_output_profile_t output_profile[] =
{
    {ENDAP_OUTPUT_ID(0), "Output 100", "GPIO 2 • relay channel • active-low", GPIO_NUM_2, true},
    {ENDAP_OUTPUT_ID(1), "Output 101", "GPIO 4 • relay channel • active-low", GPIO_NUM_4, true},
    {ENDAP_OUTPUT_ID(2), "Output 102", "GPIO 5 • relay channel • active-low", GPIO_NUM_5, true},
    {ENDAP_OUTPUT_ID(3), "Output 103", "Canal adicional • escolha um GPIO pela dashboard", GPIO_NUM_NC, false},
    {ENDAP_OUTPUT_ID(4), "Output 104", "Canal adicional • escolha um GPIO pela dashboard", GPIO_NUM_NC, false},
    {ENDAP_OUTPUT_ID(5), "Output 105", "Canal adicional • escolha um GPIO pela dashboard", GPIO_NUM_NC, false},
    {ENDAP_OUTPUT_ID(6), "Output 106", "Canal adicional • escolha um GPIO pela dashboard", GPIO_NUM_NC, false},
    {ENDAP_OUTPUT_ID(7), "Output 107", "Canal adicional • escolha um GPIO pela dashboard", GPIO_NUM_NC, false},
    {ENDAP_OUTPUT_ID(8), "Output 108", "Canal adicional • escolha um GPIO pela dashboard", GPIO_NUM_NC, false},
    {ENDAP_OUTPUT_ID(9), "Output 109", "Canal adicional • escolha um GPIO pela dashboard", GPIO_NUM_NC, false},
    {ENDAP_OUTPUT_ID(10), "Output 110", "Canal adicional • escolha um GPIO pela dashboard", GPIO_NUM_NC, false},
    {ENDAP_OUTPUT_ID(11), "Output 111", "Canal adicional • escolha um GPIO pela dashboard", GPIO_NUM_NC, false},
    {ENDAP_OUTPUT_ID(12), "Output 112", "Canal adicional • escolha um GPIO pela dashboard", GPIO_NUM_NC, false},
    {ENDAP_OUTPUT_ID(13), "Output 113", "Canal adicional • escolha um GPIO pela dashboard", GPIO_NUM_NC, false},
    {ENDAP_OUTPUT_ID(14), "Output 114", "Canal adicional • escolha um GPIO pela dashboard", GPIO_NUM_NC, false},
    {ENDAP_OUTPUT_ID(15), "Output 115", "Canal adicional • escolha um GPIO pela dashboard", GPIO_NUM_NC, false},
};

static const device_default_automation_t default_automation[] =
{
    {ENDAP_INPUT_0_ID, ENDAP_OUTPUT_0_ID, 0},
    {ENDAP_INPUT_1_ID, ENDAP_OUTPUT_1_ID, 0},
};

static const device_gpio_option_t input_gpio_options[] =
{
    {GPIO_NUM_16, "GPIO16"},
    {GPIO_NUM_17, "GPIO17"},
    {GPIO_NUM_18, "GPIO18"},
    {GPIO_NUM_19, "GPIO19"},
    {GPIO_NUM_21, "GPIO21"},
    {GPIO_NUM_22, "GPIO22"},
    {GPIO_NUM_23, "GPIO23"},
    {GPIO_NUM_25, "GPIO25"},
    {GPIO_NUM_26, "GPIO26"},
    {GPIO_NUM_27, "GPIO27"},
    {GPIO_NUM_32, "GPIO32"},
    {GPIO_NUM_33, "GPIO33"},
    {GPIO_NUM_34, "GPIO34 (entrada apenas)"},
    {GPIO_NUM_35, "GPIO35 (entrada apenas)"},
    {GPIO_NUM_36, "GPIO36 / VP (entrada apenas)"},
    {GPIO_NUM_39, "GPIO39 / VN (entrada apenas)"},
};

static const device_gpio_option_t output_gpio_options[] =
{
    {GPIO_NUM_2, "GPIO2"},
    {GPIO_NUM_4, "GPIO4"},
    {GPIO_NUM_5, "GPIO5"},
    {GPIO_NUM_13, "GPIO13"},
    {GPIO_NUM_14, "GPIO14"},
    {GPIO_NUM_16, "GPIO16"},
    {GPIO_NUM_17, "GPIO17"},
    {GPIO_NUM_18, "GPIO18"},
    {GPIO_NUM_19, "GPIO19"},
    {GPIO_NUM_21, "GPIO21"},
    {GPIO_NUM_22, "GPIO22"},
    {GPIO_NUM_23, "GPIO23"},
    {GPIO_NUM_25, "GPIO25"},
    {GPIO_NUM_26, "GPIO26"},
    {GPIO_NUM_27, "GPIO27"},
    {GPIO_NUM_32, "GPIO32"},
    {GPIO_NUM_33, "GPIO33"},
};

static device_network_profile_t network_profile = {0};
static bool network_profile_initialized = false;
static bool network_profile_initializing = false;

static const device_node_capabilities_t node_capabilities =
{
    .local_input_slot_capacity = (uint16_t)ARRAY_LEN(input_profile),
    .local_output_slot_capacity = (uint16_t)ARRAY_LEN(output_profile),
    .default_input_count = (uint16_t)ENDAP_DEFAULT_ACTIVE_INPUTS,
    .default_output_count = (uint16_t)ENDAP_DEFAULT_ACTIVE_OUTPUTS,
    .distributed_scaling = true,
    .supports_remote_nodes = true,
    .supports_mcp_digital = true,
    .supports_native_analog = true,
    .supports_external_analog = true,
    .global_capacity_mode = "distributed-sum-of-nodes",
    .local_capacity_mode = "per-node-profile",
    .recommended_scaling_path = "Expandir com novos nos antes de saturar o IO local; usar MCP para digital local e ADC/expansor para analogico quando fizer sentido.",
};

static const device_expansion_capabilities_t expansion_capabilities =
{
    .supports_mcp23x17 = true,
    .recommended_mcp_instances = 2,
    .channels_per_mcp = 16,
    .supports_ads1115 = true,
    .recommended_external_adc_instances = 2,
    .channels_per_external_adc = 4,
    .native_analog_input_channels = 6,
    .notes = "GPIO nativo continua sendo a base. MCP23x17 e ADCs externos entram como expansao local opcional por perfil de no.",
};

static const device_channel_inventory_group_t channel_inventory_groups[] =
{
    {
        .group_id = "native-digital-input",
        .label = "Entradas digitais nativas",
        .channel_class = DEVICE_CHANNEL_CLASS_DIGITAL_INPUT,
        .backend = DEVICE_CHANNEL_BACKEND_GPIO,
        .nominal_capacity = (uint16_t)ARRAY_LEN(input_profile),
        .default_slots = (uint16_t)ENDAP_DEFAULT_ACTIVE_INPUTS,
        .implemented_now = true,
        .dashboard_ready = true,
        .expansion_path = false,
        .addressing_model = "GPIOx",
        .notes = "Slots locais baseados em GPIO do proprio ESP32. Esta e a camada local ja funcional do ENDAP.",
    },
    {
        .group_id = "native-digital-output",
        .label = "Saidas digitais nativas",
        .channel_class = DEVICE_CHANNEL_CLASS_DIGITAL_OUTPUT,
        .backend = DEVICE_CHANNEL_BACKEND_GPIO,
        .nominal_capacity = (uint16_t)ARRAY_LEN(output_profile),
        .default_slots = (uint16_t)ENDAP_DEFAULT_ACTIVE_OUTPUTS,
        .implemented_now = true,
        .dashboard_ready = true,
        .expansion_path = false,
        .addressing_model = "GPIOx",
        .notes = "Saidas locais para rele e atuador diretamente no no atual.",
    },
    {
        .group_id = "mcp-digital-input",
        .label = "Entradas digitais expandidas",
        .channel_class = DEVICE_CHANNEL_CLASS_DIGITAL_INPUT,
        .backend = DEVICE_CHANNEL_BACKEND_MCP23X17,
        .nominal_capacity = 16U,
        .default_slots = 0U,
        .implemented_now = false,
        .dashboard_ready = false,
        .expansion_path = true,
        .addressing_model = "MCPn:P0..P15",
        .notes = "Expansao local opcional para aumentar pontos digitais sem consumir tanto GPIO nativo do ESP32.",
    },
    {
        .group_id = "mcp-digital-output",
        .label = "Saidas digitais expandidas",
        .channel_class = DEVICE_CHANNEL_CLASS_DIGITAL_OUTPUT,
        .backend = DEVICE_CHANNEL_BACKEND_MCP23X17,
        .nominal_capacity = 16U,
        .default_slots = 0U,
        .implemented_now = false,
        .dashboard_ready = false,
        .expansion_path = true,
        .addressing_model = "MCPn:P0..P15",
        .notes = "Pensado para modulos locais de rele e atuacao quando o perfil do no pedir mais saidas digitais.",
    },
    {
        .group_id = "native-analog-input",
        .label = "Entradas analogicas nativas",
        .channel_class = DEVICE_CHANNEL_CLASS_ANALOG_INPUT,
        .backend = DEVICE_CHANNEL_BACKEND_ADC_NATIVE,
        .nominal_capacity = 6U,
        .default_slots = 0U,
        .implemented_now = false,
        .dashboard_ready = false,
        .expansion_path = true,
        .addressing_model = "ADC1:CHx",
        .notes = "Canal analogico local previsto para sensores simples no proprio ESP32, respeitando limitacoes e ruido do hardware.",
    },
    {
        .group_id = "external-analog-input",
        .label = "Entradas analogicas expandidas",
        .channel_class = DEVICE_CHANNEL_CLASS_ANALOG_INPUT,
        .backend = DEVICE_CHANNEL_BACKEND_ADC_EXTERNAL,
        .nominal_capacity = 4U,
        .default_slots = 0U,
        .implemented_now = false,
        .dashboard_ready = false,
        .expansion_path = true,
        .addressing_model = "ADCn:A0..A3",
        .notes = "Pensado para modulos ADC externos dedicados quando a qualidade ou a quantidade de sinais analogicos exigir uma camada propria.",
    },
};

typedef struct
{
    uint32_t magic;
    uint16_t version;
    uint8_t wifi_enabled;
    uint8_t ethernet_enabled;
    uint8_t rs485_enabled;
    uint8_t onboarding_pending;
    uint8_t primary_transport;
    uint8_t fallback_transport;
    uint16_t reserved0;
    uint32_t failover_delay_ms;
    uint32_t recovery_hysteresis_ms;
    uint32_t crc;
} device_profile_network_blob_v3_t;

typedef struct
{
    uint32_t magic;
    uint16_t version;
    uint8_t wifi_enabled;
    uint8_t ethernet_enabled;
    uint8_t onboarding_pending;
    uint8_t primary_transport;
    uint8_t fallback_transport;
    uint8_t reserved0;
    uint32_t failover_delay_ms;
    uint32_t recovery_hysteresis_ms;
    uint32_t crc;
} device_profile_network_blob_v2_t;

typedef struct
{
    uint32_t magic;
    uint16_t version;
    uint8_t wifi_enabled;
    uint8_t ethernet_enabled;
    uint32_t crc;
} device_profile_network_blob_v1_t;

static uint32_t device_profile_crc32(const uint8_t *data, size_t len)
{
    uint32_t crc = 0xFFFFFFFFU;

    for (size_t i = 0; i < len; i++)
    {
        crc ^= data[i];

        for (int bit = 0; bit < 8; bit++)
            crc = (crc & 1U) ? ((crc >> 1U) ^ 0xEDB88320U) : (crc >> 1U);
    }

    return ~crc;
}

static gpio_num_t gpio_from_config(int value)
{
    if (value < 0)
        return GPIO_NUM_NC;

    return (gpio_num_t)value;
}

static bool device_profile_gpio_reserved_for_platform(gpio_num_t gpio)
{
    const device_network_profile_t *network = device_profile_network();

    if (!network || !network->rs485_supported || !network->rs485_enabled)
        return false;

    return gpio == GPIO_NUM_25 ||
           gpio == GPIO_NUM_26 ||
           gpio == GPIO_NUM_27;
}

static bool device_profile_gpio_is_listed(const device_gpio_option_t *options, int count, gpio_num_t gpio)
{
    for (int i = 0; i < count; i++)
    {
        if (options[i].gpio == gpio)
            return true;
    }

    return false;
}

static device_profile_ethernet_mode_t device_profile_detect_ethernet_mode(void)
{
#if CONFIG_ENDAP_ETH_BACKEND_W5500
    return DEVICE_PROFILE_ETH_SPI_W5500;
#elif CONFIG_ENDAP_ETH_BACKEND_LAN8720
    return DEVICE_PROFILE_ETH_INTERNAL_LAN8720;
#else
    return DEVICE_PROFILE_ETH_NONE;
#endif
}

static bool device_profile_transport_allowed_in_network(device_profile_transport_t transport,
                                                        const device_network_profile_t *network)
{
    if (!network)
        return false;

    switch (transport)
    {
        case DEVICE_PROFILE_TRANSPORT_WIFI:
            return network->wifi_supported;
        case DEVICE_PROFILE_TRANSPORT_ETHERNET:
            return network->ethernet_supported;
        case DEVICE_PROFILE_TRANSPORT_RS485:
            return network->rs485_supported && network->rs485_enabled;
        case DEVICE_PROFILE_TRANSPORT_NONE:
        default:
            return true;
    }
}

static bool device_profile_w5500_is_configured_from_profile(const device_network_profile_t *network)
{
    const device_network_w5500_profile_t *cfg;

    if (!network || network->ethernet_mode != DEVICE_PROFILE_ETH_SPI_W5500)
        return false;

    cfg = &network->w5500;

    return cfg->mosi_gpio != GPIO_NUM_NC &&
           cfg->miso_gpio != GPIO_NUM_NC &&
           cfg->sclk_gpio != GPIO_NUM_NC &&
           cfg->cs_gpio != GPIO_NUM_NC;
}

static void device_profile_apply_legacy_transport_policy(bool wifi_enabled,
                                                         bool ethernet_enabled)
{
    network_profile.onboarding_pending = false;
    network_profile.rs485_enabled = false;

    if (wifi_enabled && ethernet_enabled)
    {
        network_profile.primary_transport = DEVICE_PROFILE_TRANSPORT_ETHERNET;
        network_profile.fallback_transport = DEVICE_PROFILE_TRANSPORT_WIFI;
    }
    else if (ethernet_enabled)
    {
        network_profile.primary_transport = DEVICE_PROFILE_TRANSPORT_ETHERNET;
        network_profile.fallback_transport = DEVICE_PROFILE_TRANSPORT_NONE;
    }
    else if (wifi_enabled)
    {
        network_profile.primary_transport = DEVICE_PROFILE_TRANSPORT_WIFI;
        network_profile.fallback_transport = DEVICE_PROFILE_TRANSPORT_NONE;
    }
    else
    {
        network_profile.primary_transport = DEVICE_PROFILE_TRANSPORT_NONE;
        network_profile.fallback_transport = DEVICE_PROFILE_TRANSPORT_NONE;
    }
}

static void device_profile_apply_manual_network_selection(bool wifi_enabled,
                                                          bool ethernet_enabled,
                                                          bool rs485_enabled)
{
    const bool wifi_allowed = network_profile.wifi_supported && wifi_enabled;
    const bool ethernet_allowed = network_profile.ethernet_supported && ethernet_enabled;
    const bool rs485_allowed = network_profile.rs485_supported && rs485_enabled;

    network_profile.onboarding_pending = false;
    network_profile.wifi_enabled = wifi_allowed;
    network_profile.ethernet_enabled = ethernet_allowed;
    network_profile.rs485_enabled = rs485_allowed;
    network_profile.primary_transport = DEVICE_PROFILE_TRANSPORT_NONE;
    network_profile.fallback_transport = DEVICE_PROFILE_TRANSPORT_NONE;

    if (ethernet_allowed)
        network_profile.primary_transport = DEVICE_PROFILE_TRANSPORT_ETHERNET;
    else if (wifi_allowed)
        network_profile.primary_transport = DEVICE_PROFILE_TRANSPORT_WIFI;
    else if (rs485_allowed)
        network_profile.primary_transport = DEVICE_PROFILE_TRANSPORT_RS485;

    if (network_profile.primary_transport != DEVICE_PROFILE_TRANSPORT_ETHERNET && ethernet_allowed)
        network_profile.fallback_transport = DEVICE_PROFILE_TRANSPORT_ETHERNET;
    else if (network_profile.primary_transport != DEVICE_PROFILE_TRANSPORT_WIFI && wifi_allowed)
        network_profile.fallback_transport = DEVICE_PROFILE_TRANSPORT_WIFI;
    else if (network_profile.primary_transport != DEVICE_PROFILE_TRANSPORT_RS485 && rs485_allowed)
        network_profile.fallback_transport = DEVICE_PROFILE_TRANSPORT_RS485;
}

static void device_profile_normalize_transport_policy(void)
{
    if (network_profile.failover_delay_ms == 0U)
        network_profile.failover_delay_ms = DEVICE_PROFILE_FAILOVER_DELAY_DEFAULT_MS;

    if (network_profile.recovery_hysteresis_ms == 0U)
        network_profile.recovery_hysteresis_ms = DEVICE_PROFILE_RECOVERY_HYST_DEFAULT_MS;

    if (network_profile.onboarding_pending)
    {
        network_profile.primary_transport = network_profile.wifi_supported
            ? DEVICE_PROFILE_TRANSPORT_WIFI
            : (network_profile.ethernet_supported
                ? DEVICE_PROFILE_TRANSPORT_ETHERNET
                : DEVICE_PROFILE_TRANSPORT_NONE);

        network_profile.fallback_transport = DEVICE_PROFILE_TRANSPORT_NONE;
        network_profile.wifi_enabled = network_profile.wifi_supported;
        network_profile.ethernet_enabled = false;
        network_profile.rs485_enabled = false;
        return;
    }

    if (!device_profile_transport_allowed_in_network(network_profile.primary_transport, &network_profile))
    {
        if (network_profile.ethernet_supported)
            network_profile.primary_transport = DEVICE_PROFILE_TRANSPORT_ETHERNET;
        else if (network_profile.wifi_supported)
            network_profile.primary_transport = DEVICE_PROFILE_TRANSPORT_WIFI;
        else if (network_profile.rs485_supported && network_profile.rs485_enabled)
            network_profile.primary_transport = DEVICE_PROFILE_TRANSPORT_RS485;
        else
            network_profile.primary_transport = DEVICE_PROFILE_TRANSPORT_NONE;
    }

    if (!device_profile_transport_allowed_in_network(network_profile.fallback_transport, &network_profile) ||
        network_profile.fallback_transport == network_profile.primary_transport)
    {
        network_profile.fallback_transport = DEVICE_PROFILE_TRANSPORT_NONE;
    }

    switch (network_profile.primary_transport)
    {
        case DEVICE_PROFILE_TRANSPORT_ETHERNET:
            network_profile.ethernet_enabled = network_profile.ethernet_supported;
            network_profile.wifi_enabled = (network_profile.fallback_transport == DEVICE_PROFILE_TRANSPORT_WIFI);
            break;
        case DEVICE_PROFILE_TRANSPORT_WIFI:
            network_profile.wifi_enabled = network_profile.wifi_supported;
            network_profile.ethernet_enabled = (network_profile.fallback_transport == DEVICE_PROFILE_TRANSPORT_ETHERNET);
            break;
        case DEVICE_PROFILE_TRANSPORT_RS485:
            network_profile.wifi_enabled = false;
            network_profile.ethernet_enabled = false;
            break;
        case DEVICE_PROFILE_TRANSPORT_NONE:
        default:
            network_profile.wifi_enabled = false;
            network_profile.ethernet_enabled = false;
            break;
    }

    if (!network_profile.rs485_supported)
    {
        network_profile.rs485_enabled = false;
    }
    else if (network_profile.primary_transport == DEVICE_PROFILE_TRANSPORT_RS485 ||
             network_profile.fallback_transport == DEVICE_PROFILE_TRANSPORT_RS485)
    {
        network_profile.rs485_enabled = true;
    }
}

static const char *device_profile_network_label_from_profile(const device_network_profile_t *network)
{
    if (!network)
        return "network-disabled";

    if (network->onboarding_pending)
        return "wifi-onboarding";

    if (network->primary_transport == DEVICE_PROFILE_TRANSPORT_ETHERNET &&
        network->fallback_transport == DEVICE_PROFILE_TRANSPORT_WIFI)
        return "ethernet-primary / wifi-fallback";

    if (network->primary_transport == DEVICE_PROFILE_TRANSPORT_ETHERNET &&
        network->fallback_transport == DEVICE_PROFILE_TRANSPORT_RS485)
        return "ethernet-primary / rs485-fallback";

    if (network->primary_transport == DEVICE_PROFILE_TRANSPORT_WIFI &&
        network->fallback_transport == DEVICE_PROFILE_TRANSPORT_ETHERNET)
        return "wifi-primary / ethernet-fallback";

    if (network->primary_transport == DEVICE_PROFILE_TRANSPORT_WIFI &&
        network->fallback_transport == DEVICE_PROFILE_TRANSPORT_RS485)
        return "wifi-primary / rs485-fallback";

    if (network->primary_transport == DEVICE_PROFILE_TRANSPORT_ETHERNET)
        return device_profile_w5500_is_configured_from_profile(network)
            ? "ethernet-primary"
            : "ethernet-setup";

    if (network->primary_transport == DEVICE_PROFILE_TRANSPORT_WIFI)
        return "wifi-primary";

    if (network->primary_transport == DEVICE_PROFILE_TRANSPORT_RS485)
        return "rs485-primary";

    if (network->rs485_enabled)
        return "rs485-runtime";

    return "network-disabled";
}

static bool device_profile_gpio_reserved_for_network(gpio_num_t gpio)
{
    const device_network_profile_t *network = device_profile_network();
    const device_network_w5500_profile_t *w5500;

    if (!network)
        return false;

    if (!network->ethernet_supported || !network->ethernet_enabled)
        return false;

    if (!device_profile_w5500_is_configured_from_profile(network))
        return false;

    w5500 = &network->w5500;

    return gpio == w5500->mosi_gpio ||
           gpio == w5500->miso_gpio ||
           gpio == w5500->sclk_gpio ||
           gpio == w5500->cs_gpio ||
           gpio == w5500->int_gpio ||
           gpio == w5500->reset_gpio;
}

static void device_profile_load_network_config(void)
{
    uint8_t raw[sizeof(device_profile_network_blob_v3_t)] = {0};
    nvs_handle_t nvs;
    size_t len = sizeof(raw);

    if (nvs_open(DEVICE_PROFILE_NETWORK_NAMESPACE, NVS_READONLY, &nvs) != ESP_OK)
        return;

    if (nvs_get_blob(nvs, DEVICE_PROFILE_NETWORK_KEY, raw, &len) != ESP_OK)
    {
        nvs_close(nvs);
        return;
    }

    nvs_close(nvs);

    if (len == sizeof(device_profile_network_blob_v3_t))
    {
        const device_profile_network_blob_v3_t *blob = (const device_profile_network_blob_v3_t *)raw;
        uint32_t crc = device_profile_crc32(raw, sizeof(*blob) - sizeof(blob->crc));

        if (blob->magic != DEVICE_PROFILE_NETWORK_MAGIC ||
            blob->version != DEVICE_PROFILE_NETWORK_VERSION ||
            blob->crc != crc)
        {
            ESP_LOGW(TAG, "Blob de rede v3 invalido; usando defaults do build");
            return;
        }

        network_profile.wifi_enabled = network_profile.wifi_supported ? (blob->wifi_enabled != 0U) : false;
        network_profile.ethernet_enabled = network_profile.ethernet_supported ? (blob->ethernet_enabled != 0U) : false;
        network_profile.rs485_enabled = network_profile.rs485_supported ? (blob->rs485_enabled != 0U) : false;
        network_profile.onboarding_pending = (blob->onboarding_pending != 0U);
        network_profile.primary_transport = (device_profile_transport_t)blob->primary_transport;
        network_profile.fallback_transport = (device_profile_transport_t)blob->fallback_transport;
        network_profile.failover_delay_ms = blob->failover_delay_ms;
        network_profile.recovery_hysteresis_ms = blob->recovery_hysteresis_ms;
        return;
    }

    if (len == sizeof(device_profile_network_blob_v2_t))
    {
        const device_profile_network_blob_v2_t *blob = (const device_profile_network_blob_v2_t *)raw;
        uint32_t crc = device_profile_crc32(raw, sizeof(*blob) - sizeof(blob->crc));

        if (blob->magic != DEVICE_PROFILE_NETWORK_MAGIC ||
            blob->version != DEVICE_PROFILE_NETWORK_VERSION ||
            blob->crc != crc)
        {
            ESP_LOGW(TAG, "Blob de rede v2 invalido; usando defaults do build");
            return;
        }

        network_profile.wifi_enabled = network_profile.wifi_supported ? (blob->wifi_enabled != 0U) : false;
        network_profile.ethernet_enabled = network_profile.ethernet_supported ? (blob->ethernet_enabled != 0U) : false;
        network_profile.rs485_enabled = false;
        network_profile.onboarding_pending = (blob->onboarding_pending != 0U);
        network_profile.primary_transport = (device_profile_transport_t)blob->primary_transport;
        network_profile.fallback_transport = (device_profile_transport_t)blob->fallback_transport;
        network_profile.failover_delay_ms = blob->failover_delay_ms;
        network_profile.recovery_hysteresis_ms = blob->recovery_hysteresis_ms;
        return;
    }

    if (len == sizeof(device_profile_network_blob_v1_t))
    {
        const device_profile_network_blob_v1_t *blob = (const device_profile_network_blob_v1_t *)raw;
        uint32_t crc = device_profile_crc32(raw, sizeof(*blob) - sizeof(blob->crc));

        if (blob->magic != DEVICE_PROFILE_NETWORK_MAGIC ||
            blob->version != 1U ||
            blob->crc != crc ||
            blob->wifi_enabled > 1U ||
            blob->ethernet_enabled > 1U)
        {
            ESP_LOGW(TAG, "Blob de rede v1 invalido; usando defaults do build");
            return;
        }

        network_profile.wifi_enabled = network_profile.wifi_supported ? (blob->wifi_enabled != 0U) : false;
        network_profile.ethernet_enabled = network_profile.ethernet_supported ? (blob->ethernet_enabled != 0U) : false;
        network_profile.rs485_enabled = false;
        device_profile_apply_legacy_transport_policy(network_profile.wifi_enabled,
                                                     network_profile.ethernet_enabled);
        return;
    }

    ESP_LOGW(TAG, "Blob de rede com tamanho inesperado; usando defaults do build");
}

static void device_profile_init_network_profile(void)
{
    if (network_profile_initialized)
        return;

    if (network_profile_initializing)
        return;

    network_profile_initializing = true;
    memset(&network_profile, 0, sizeof(network_profile));

    network_profile.wifi_supported = CONFIG_ENDAP_WIFI_ENABLED;
    network_profile.ethernet_supported = CONFIG_ENDAP_ETHERNET_ENABLED;
    network_profile.rs485_supported = true;
    network_profile.wifi_enabled = network_profile.wifi_supported;
    network_profile.ethernet_enabled = false;
    network_profile.rs485_enabled = false;
    network_profile.onboarding_pending = true;
    network_profile.primary_transport = network_profile.wifi_supported
        ? DEVICE_PROFILE_TRANSPORT_WIFI
        : (network_profile.ethernet_supported
            ? DEVICE_PROFILE_TRANSPORT_ETHERNET
            : DEVICE_PROFILE_TRANSPORT_NONE);
    network_profile.fallback_transport = DEVICE_PROFILE_TRANSPORT_NONE;
    network_profile.failover_delay_ms = DEVICE_PROFILE_FAILOVER_DELAY_DEFAULT_MS;
    network_profile.recovery_hysteresis_ms = DEVICE_PROFILE_RECOVERY_HYST_DEFAULT_MS;
    network_profile.ethernet_mode = device_profile_detect_ethernet_mode();
    network_profile.w5500.spi_host_id = CONFIG_ENDAP_W5500_SPI_HOST;
    network_profile.w5500.mosi_gpio = gpio_from_config(CONFIG_ENDAP_W5500_MOSI_GPIO);
    network_profile.w5500.miso_gpio = gpio_from_config(CONFIG_ENDAP_W5500_MISO_GPIO);
    network_profile.w5500.sclk_gpio = gpio_from_config(CONFIG_ENDAP_W5500_SCLK_GPIO);
    network_profile.w5500.cs_gpio = gpio_from_config(CONFIG_ENDAP_W5500_CS_GPIO);
    network_profile.w5500.int_gpio = gpio_from_config(CONFIG_ENDAP_W5500_INT_GPIO);
    network_profile.w5500.reset_gpio = gpio_from_config(CONFIG_ENDAP_W5500_RESET_GPIO);
    network_profile.w5500.phy_addr = CONFIG_ENDAP_W5500_PHY_ADDR;
    network_profile.w5500.clock_mhz = CONFIG_ENDAP_W5500_CLOCK_MHZ;

    device_profile_load_network_config();
    device_profile_normalize_transport_policy();
    network_profile.label = device_profile_network_label_from_profile(&network_profile);

    network_profile_initialized = true;
    network_profile_initializing = false;
}

static device_profile_network_config_result_t device_profile_persist_network_config(void)
{
    device_profile_network_blob_v3_t blob = {0};
    nvs_handle_t nvs;

    blob.magic = DEVICE_PROFILE_NETWORK_MAGIC;
    blob.version = DEVICE_PROFILE_NETWORK_VERSION;
    blob.wifi_enabled = network_profile.wifi_enabled ? 1U : 0U;
    blob.ethernet_enabled = network_profile.ethernet_enabled ? 1U : 0U;
    blob.rs485_enabled = network_profile.rs485_enabled ? 1U : 0U;
    blob.onboarding_pending = network_profile.onboarding_pending ? 1U : 0U;
    blob.primary_transport = (uint8_t)network_profile.primary_transport;
    blob.fallback_transport = (uint8_t)network_profile.fallback_transport;
    blob.failover_delay_ms = network_profile.failover_delay_ms;
    blob.recovery_hysteresis_ms = network_profile.recovery_hysteresis_ms;
    blob.crc = device_profile_crc32((const uint8_t *)&blob, sizeof(blob) - sizeof(blob.crc));

    if (nvs_open(DEVICE_PROFILE_NETWORK_NAMESPACE, NVS_READWRITE, &nvs) != ESP_OK)
    {
        ESP_LOGE(TAG, "Falha ao abrir NVS para persistir configuracao de rede");
        return DEVICE_PROFILE_NETWORK_CONFIG_PERSIST_FAILED;
    }

    if (nvs_set_blob(nvs, DEVICE_PROFILE_NETWORK_KEY, &blob, sizeof(blob)) != ESP_OK ||
        nvs_commit(nvs) != ESP_OK)
    {
        nvs_close(nvs);
        ESP_LOGE(TAG, "Falha ao persistir configuracao de rede");
        return DEVICE_PROFILE_NETWORK_CONFIG_PERSIST_FAILED;
    }

    nvs_close(nvs);
    return DEVICE_PROFILE_NETWORK_CONFIG_OK;
}

int device_profile_input_count(void)
{
    return ARRAY_LEN(input_profile);
}

int device_profile_input_capacity(void)
{
    return ARRAY_LEN(input_profile);
}

const device_input_profile_t *device_profile_input_at(int index)
{
    if (index < 0 || index >= ARRAY_LEN(input_profile))
        return NULL;

    return &input_profile[index];
}

uint16_t device_profile_input_id_at(int index)
{
    const device_input_profile_t *input = device_profile_input_at(index);
    return input ? input->id : 0U;
}

const device_input_profile_t *device_profile_find_input(uint16_t id)
{
    for (int i = 0; i < ARRAY_LEN(input_profile); i++)
    {
        if (input_profile[i].id == id)
            return &input_profile[i];
    }

    return NULL;
}

bool device_profile_is_valid_input(uint16_t id)
{
    return device_profile_find_input(id) != NULL;
}

int device_profile_input_gpio_option_count(void)
{
    return ARRAY_LEN(input_gpio_options);
}

const device_gpio_option_t *device_profile_input_gpio_option_at(int index)
{
    if (index < 0 || index >= ARRAY_LEN(input_gpio_options))
        return NULL;

    return &input_gpio_options[index];
}

bool device_profile_input_gpio_allowed(gpio_num_t gpio)
{
    return device_profile_gpio_is_listed(input_gpio_options, ARRAY_LEN(input_gpio_options), gpio) &&
           !device_profile_gpio_reserved_for_platform(gpio) &&
           !device_profile_gpio_reserved_for_network(gpio);
}

int device_profile_output_count(void)
{
    return ARRAY_LEN(output_profile);
}

int device_profile_output_capacity(void)
{
    return ARRAY_LEN(output_profile);
}

const device_output_profile_t *device_profile_output_at(int index)
{
    if (index < 0 || index >= ARRAY_LEN(output_profile))
        return NULL;

    return &output_profile[index];
}

uint16_t device_profile_output_id_at(int index)
{
    const device_output_profile_t *output = device_profile_output_at(index);
    return output ? output->id : 0U;
}

const device_output_profile_t *device_profile_find_output(uint16_t id)
{
    for (int i = 0; i < ARRAY_LEN(output_profile); i++)
    {
        if (output_profile[i].id == id)
            return &output_profile[i];
    }

    return NULL;
}

bool device_profile_is_valid_output(uint16_t id)
{
    return device_profile_find_output(id) != NULL;
}

int device_profile_output_gpio_option_count(void)
{
    return ARRAY_LEN(output_gpio_options);
}

const device_gpio_option_t *device_profile_output_gpio_option_at(int index)
{
    if (index < 0 || index >= ARRAY_LEN(output_gpio_options))
        return NULL;

    return &output_gpio_options[index];
}

bool device_profile_output_gpio_allowed(gpio_num_t gpio)
{
    return device_profile_gpio_is_listed(output_gpio_options, ARRAY_LEN(output_gpio_options), gpio) &&
           !device_profile_gpio_reserved_for_platform(gpio) &&
           !device_profile_gpio_reserved_for_network(gpio);
}

uint16_t device_profile_preferred_self_test_output_id(void)
{
    for (int i = (int)ENDAP_DEFAULT_ACTIVE_OUTPUTS - 1; i >= 0; i--)
    {
        const device_output_profile_t *preferred = device_profile_output_at(i);
        if (preferred && preferred->id != 0U)
            return preferred->id;
    }

    return 0U;
}

const device_node_capabilities_t *device_profile_node_capabilities(void)
{
    return &node_capabilities;
}

const device_expansion_capabilities_t *device_profile_expansion_capabilities(void)
{
    return &expansion_capabilities;
}

int device_profile_channel_inventory_group_count(void)
{
    return ARRAY_LEN(channel_inventory_groups);
}

const device_channel_inventory_group_t *device_profile_channel_inventory_group_at(int index)
{
    if (index < 0 || index >= ARRAY_LEN(channel_inventory_groups))
        return NULL;

    return &channel_inventory_groups[index];
}

const device_network_profile_t *device_profile_network(void)
{
    device_profile_init_network_profile();
    return &network_profile;
}

const device_network_w5500_profile_t *device_profile_w5500(void)
{
    return &device_profile_network()->w5500;
}

bool device_profile_w5500_is_configured(void)
{
    device_profile_init_network_profile();
    return device_profile_w5500_is_configured_from_profile(&network_profile);
}

bool device_profile_supports_wifi(void)
{
    const device_network_profile_t *network = device_profile_network();
    return network->wifi_supported && network->wifi_enabled;
}

bool device_profile_supports_ethernet(void)
{
    const device_network_profile_t *network = device_profile_network();
    return network->ethernet_supported && network->ethernet_enabled;
}

bool device_profile_supports_rs485(void)
{
    return device_profile_transport_enabled(DEVICE_PROFILE_TRANSPORT_RS485);
}

bool device_profile_transport_supported(device_profile_transport_t transport)
{
    const device_network_profile_t *network = device_profile_network();

    if (!network)
        return false;

    switch (transport)
    {
        case DEVICE_PROFILE_TRANSPORT_WIFI:
            return network->wifi_supported;
        case DEVICE_PROFILE_TRANSPORT_ETHERNET:
            return network->ethernet_supported;
        case DEVICE_PROFILE_TRANSPORT_RS485:
            return network->rs485_supported;
        case DEVICE_PROFILE_TRANSPORT_NONE:
        default:
            return true;
    }
}

bool device_profile_transport_enabled(device_profile_transport_t transport)
{
    const device_network_profile_t *network = device_profile_network();

    if (!device_profile_transport_allowed_in_network(transport, network))
        return false;

    switch (transport)
    {
        case DEVICE_PROFILE_TRANSPORT_WIFI:
            return network->wifi_enabled;
        case DEVICE_PROFILE_TRANSPORT_ETHERNET:
            return network->ethernet_enabled;
        case DEVICE_PROFILE_TRANSPORT_RS485:
            return network->rs485_enabled;
        case DEVICE_PROFILE_TRANSPORT_NONE:
        default:
            return true;
    }
}

bool device_profile_transport_visible(device_profile_transport_t transport)
{
    if (transport == DEVICE_PROFILE_TRANSPORT_NONE)
        return true;

    return device_profile_transport_enabled(transport);
}

bool device_profile_network_onboarding_pending(void)
{
    return device_profile_network()->onboarding_pending;
}

bool device_profile_should_start_wifi_on_boot(void)
{
    const device_network_profile_t *network = device_profile_network();

    if (network->onboarding_pending)
        return device_profile_transport_supported(DEVICE_PROFILE_TRANSPORT_WIFI);

    return device_profile_transport_enabled(DEVICE_PROFILE_TRANSPORT_WIFI) &&
           network->primary_transport == DEVICE_PROFILE_TRANSPORT_WIFI;
}

bool device_profile_should_start_ethernet_on_boot(void)
{
    const device_network_profile_t *network = device_profile_network();

    if (network->onboarding_pending)
        return false;

    return device_profile_transport_enabled(DEVICE_PROFILE_TRANSPORT_ETHERNET) &&
           network->primary_transport == DEVICE_PROFILE_TRANSPORT_ETHERNET;
}

bool device_profile_should_start_rs485_on_boot(void)
{
    const device_network_profile_t *network = device_profile_network();
    return network->rs485_supported && network->rs485_enabled;
}

device_profile_transport_t device_profile_primary_transport(void)
{
    return device_profile_network()->primary_transport;
}

device_profile_transport_t device_profile_fallback_transport(void)
{
    return device_profile_network()->fallback_transport;
}

uint32_t device_profile_failover_delay_ms(void)
{
    return device_profile_network()->failover_delay_ms;
}

uint32_t device_profile_recovery_hysteresis_ms(void)
{
    return device_profile_network()->recovery_hysteresis_ms;
}

device_profile_network_config_result_t device_profile_set_network_enabled(bool wifi_enabled,
                                                                          bool ethernet_enabled,
                                                                          bool rs485_enabled)
{
    device_profile_init_network_profile();

    device_profile_apply_manual_network_selection(wifi_enabled,
                                                  ethernet_enabled,
                                                  rs485_enabled);
    device_profile_normalize_transport_policy();
    network_profile.label = device_profile_network_label_from_profile(&network_profile);

    ESP_LOGI(TAG,
             "Configuracao de rede manual salva: wifi=%s ethernet=%s rs485=%s => primary=%d fallback=%d onboarding=%d",
             wifi_enabled ? "on" : "off",
             ethernet_enabled ? "on" : "off",
             rs485_enabled ? "on" : "off",
             (int)network_profile.primary_transport,
             (int)network_profile.fallback_transport,
             network_profile.onboarding_pending ? 1 : 0);

    return device_profile_persist_network_config();
}

device_profile_network_config_result_t device_profile_set_transport_policy(bool onboarding_pending,
                                                                          device_profile_transport_t primary_transport,
                                                                          device_profile_transport_t fallback_transport,
                                                                          uint32_t failover_delay_ms,
                                                                          uint32_t recovery_hysteresis_ms)
{
    device_profile_init_network_profile();

    network_profile.onboarding_pending = onboarding_pending;
    network_profile.primary_transport = primary_transport;
    network_profile.fallback_transport = fallback_transport;
    network_profile.failover_delay_ms = failover_delay_ms;
    network_profile.recovery_hysteresis_ms = recovery_hysteresis_ms;

    device_profile_normalize_transport_policy();
    network_profile.label = device_profile_network_label_from_profile(&network_profile);

    ESP_LOGI(TAG,
             "Politica de transporte salva: onboarding=%s primary=%d fallback=%d failover=%" PRIu32 "ms recovery=%" PRIu32 "ms",
             network_profile.onboarding_pending ? "on" : "off",
             (int)network_profile.primary_transport,
             (int)network_profile.fallback_transport,
             network_profile.failover_delay_ms,
             network_profile.recovery_hysteresis_ms);

    return device_profile_persist_network_config();
}

bool device_profile_gpio_is_input_only(gpio_num_t gpio)
{
    return gpio == GPIO_NUM_34 ||
           gpio == GPIO_NUM_35 ||
           gpio == GPIO_NUM_36 ||
           gpio == GPIO_NUM_39;
}

int device_profile_copy_local_io_ids(uint16_t *out_ids, int max_ids)
{
    int total = ARRAY_LEN(input_profile) + ARRAY_LEN(output_profile);
    int index = 0;

    if (!out_ids || max_ids <= 0)
        return total;

    for (int i = 0; i < ARRAY_LEN(input_profile) && index < max_ids; i++)
        out_ids[index++] = input_profile[i].id;

    for (int i = 0; i < ARRAY_LEN(output_profile) && index < max_ids; i++)
        out_ids[index++] = output_profile[i].id;

    return total;
}

int device_profile_default_input_count(void)
{
    return (int)ENDAP_DEFAULT_ACTIVE_INPUTS;
}

bool device_profile_is_default_input_id(uint16_t id)
{
    for (int i = 0; i < (int)ENDAP_DEFAULT_ACTIVE_INPUTS; i++)
    {
        if (input_profile[i].id == id)
            return true;
    }

    return false;
}

bool device_profile_is_extra_input_id(uint16_t id)
{
    return device_profile_is_valid_input(id) && !device_profile_is_default_input_id(id);
}

int device_profile_default_output_count(void)
{
    return (int)ENDAP_DEFAULT_ACTIVE_OUTPUTS;
}

bool device_profile_is_default_output_id(uint16_t id)
{
    for (int i = 0; i < (int)ENDAP_DEFAULT_ACTIVE_OUTPUTS; i++)
    {
        if (output_profile[i].id == id)
            return true;
    }

    return false;
}

bool device_profile_is_extra_output_id(uint16_t id)
{
    return device_profile_is_valid_output(id) && !device_profile_is_default_output_id(id);
}

int device_profile_default_automation_count(void)
{
    return ARRAY_LEN(default_automation);
}

const device_default_automation_t *device_profile_default_automation_at(int index)
{
    if (index < 0 || index >= ARRAY_LEN(default_automation))
        return NULL;

    return &default_automation[index];
}

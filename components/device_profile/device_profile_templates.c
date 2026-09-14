#include "device_profile_templates.h"
#include "device_profile.h"

#define ARRAY_LEN(x) ((size_t)(sizeof(x) / sizeof((x)[0])))

/* -------------------------------------------------------------------- */
/*  GATEWAY PROFILE                                                     */
/* -------------------------------------------------------------------- */
static const device_node_capabilities_t gateway_node_caps = {
    .local_input_slot_capacity = 16,
    .local_output_slot_capacity = 16,
    .default_input_count = 8,
    .default_output_count = 8,
    .distributed_scaling = true,
    .supports_remote_nodes = true,
    .supports_mcp_digital = true,
    .supports_native_analog = true,
    .supports_external_analog = true,
    .global_capacity_mode = "gateway-coordinator",
    .local_capacity_mode = "gateway-full",
    .recommended_scaling_path = "Gateway centraliza rede, onboarding e dashboard; gerencia noes remotos.",
};

static const device_expansion_capabilities_t gateway_exp_caps = {
    .supports_mcp23x17 = true,
    .recommended_mcp_instances = 2,
    .channels_per_mcp = 16,
    .supports_ads1115 = true,
    .recommended_external_adc_instances = 2,
    .channels_per_external_adc = 4,
    .native_analog_input_channels = 6,
    .notes = "Gateway com capacidade total de expansao e coordenacao.",
};

static const device_network_profile_t gateway_net_profile = {
    .wifi_supported = true,
    .ethernet_supported = true,
    .rs485_supported = CONFIG_ENDAP_RS485_ENABLED,
    .wifi_enabled = true,
    .ethernet_enabled = false,
    .rs485_enabled = CONFIG_ENDAP_RS485_ENABLED,
    .wifi_mode = DEVICE_PROFILE_WIFI_MODE_INFRA,
    .onboarding_pending = false,
    .primary_transport = DEVICE_PROFILE_TRANSPORT_WIFI,
    .fallback_transport = CONFIG_ENDAP_RS485_ENABLED ? DEVICE_PROFILE_TRANSPORT_RS485 : DEVICE_PROFILE_TRANSPORT_NONE,
    .failover_delay_ms = 5000,
    .recovery_hysteresis_ms = 15000,
    .ethernet_mode = DEVICE_PROFILE_ETH_NONE,
    .allow_local_ap = true,
    .allow_dashboard = true,
    .allow_ota = true,
    .label = "gateway-network",
};

/* -------------------------------------------------------------------- */
/*  PLC CHANNELS DEFINITIONS PER PROFILE                                */
/* -------------------------------------------------------------------- */
static const plc_channel_desc_t gateway_plc_channels[] = {
    {
        .plc_code = "DI00",
        .channel_class = DEVICE_CHANNEL_CLASS_DIGITAL_INPUT,
        .channel_index = 0,
        .gpio = GPIO_NUM_18,
        .default_name = "Botão de Emergência Geral",
        .available = true,
        .role = "emergency_stop"
    },
    {
        .plc_code = "DI01",
        .channel_class = DEVICE_CHANNEL_CLASS_DIGITAL_INPUT,
        .channel_index = 1,
        .gpio = GPIO_NUM_19,
        .default_name = "Botão de Reset/Start",
        .available = true,
        .role = "start_reset"
    },
    {
        .plc_code = "DO00",
        .channel_class = DEVICE_CHANNEL_CLASS_DIGITAL_OUTPUT,
        .channel_index = 0,
        .gpio = GPIO_NUM_4,
        .default_name = "Sinalizador de Alarme Geral",
        .available = true,
        .role = "alarm"
    },
    {
        .plc_code = "DO01",
        .channel_class = DEVICE_CHANNEL_CLASS_DIGITAL_OUTPUT,
        .channel_index = 1,
        .gpio = GPIO_NUM_5,
        .default_name = "Sinalizador de Sistema em Execução",
        .available = true,
        .role = "running"
    },
    {
        .plc_code = "AI00",
        .channel_class = DEVICE_CHANNEL_CLASS_ANALOG_INPUT,
        .channel_index = 0,
        .gpio = GPIO_NUM_34,
        .default_name = "Sensor de Tensão de Linha",
        .available = true,
        .role = "voltage_sensor"
    },
    {
        .plc_code = "AO00",
        .channel_class = DEVICE_CHANNEL_CLASS_ANALOG_OUTPUT,
        .channel_index = 0,
        .gpio = GPIO_NUM_NC,
        .default_name = "Saída Analógica (Não Disponível)",
        .available = false,
        .role = "disabled"
    },
};

static const plc_channel_desc_t field_plc_channels[] = {
    /* --- DIGITAL INPUTS (DI00..DI09) --- */
    {
        .plc_code = "DI00",
        .channel_class = DEVICE_CHANNEL_CLASS_DIGITAL_INPUT,
        .channel_index = 0,
        .gpio = GPIO_NUM_16,
        .default_name = "Sensor da Porta",
        .available = true,
        .role = "door_sensor"
    },
    {
        .plc_code = "DI01",
        .channel_class = DEVICE_CHANNEL_CLASS_DIGITAL_INPUT,
        .channel_index = 1,
        .gpio = GPIO_NUM_17,
        .default_name = "Válvula de Entrada",
        .available = true,
        .role = "valve"
    },
    {
        .plc_code = "DI02",
        .channel_class = DEVICE_CHANNEL_CLASS_DIGITAL_INPUT,
        .channel_index = 2,
        .gpio = GPIO_NUM_21,
        .default_name = "Chave de Habilitação Geral",
        .available = true,
        .role = "enable_switch"
    },
    {
        .plc_code = "DI03",
        .channel_class = DEVICE_CHANNEL_CLASS_DIGITAL_INPUT,
        .channel_index = 3,
        .gpio = GPIO_NUM_22,
        .default_name = "Reset de Failsafe Local",
        .available = true,
        .role = "reset_switch"
    },
    {
        .plc_code = "DI04",
        .channel_class = DEVICE_CHANNEL_CLASS_DIGITAL_INPUT,
        .channel_index = 4,
        .gpio = GPIO_NUM_27,
        .default_name = "Boia de Nível Mínimo",
        .available = true,
        .role = "level_switch"
    },
    {
        .plc_code = "DI05",
        .channel_class = DEVICE_CHANNEL_CLASS_DIGITAL_INPUT,
        .channel_index = 5,
        .gpio = GPIO_NUM_26,
        .default_name = "Entrada Digital 05",
        .available = true,
        .role = "input"
    },
    {
        .plc_code = "DI06",
        .channel_class = DEVICE_CHANNEL_CLASS_DIGITAL_INPUT,
        .channel_index = 6,
        .gpio = GPIO_NUM_NC,
        .default_name = "Modo Auto/Manual",
        .available = false,
        .role = "auto_manual_switch"
    },
    {
        .plc_code = "DI07",
        .channel_class = DEVICE_CHANNEL_CLASS_DIGITAL_INPUT,
        .channel_index = 7,
        .gpio = GPIO_NUM_NC,
        .default_name = "Pressostato de Alta",
        .available = false,
        .role = "high_pressure_switch"
    },
    {
        .plc_code = "DI08",
        .channel_class = DEVICE_CHANNEL_CLASS_DIGITAL_INPUT,
        .channel_index = 8,
        .gpio = GPIO_NUM_NC,
        .default_name = "Pressostato de Baixa",
        .available = false,
        .role = "low_pressure_switch"
    },
    {
        .plc_code = "DI09",
        .channel_class = DEVICE_CHANNEL_CLASS_DIGITAL_INPUT,
        .channel_index = 9,
        .gpio = GPIO_NUM_NC,
        .default_name = "Fluxo de Retorno",
        .available = false,
        .role = "flow_switch"
    },

    /* --- DIGITAL OUTPUTS (DO00..DO09) --- */
    {
        .plc_code = "DO00",
        .channel_class = DEVICE_CHANNEL_CLASS_DIGITAL_OUTPUT,
        .channel_index = 0,
        .gpio = GPIO_NUM_22,
        .default_name = "Bomba Principal",
        .available = true,
        .role = "pump"
    },
    {
        .plc_code = "DO01",
        .channel_class = DEVICE_CHANNEL_CLASS_DIGITAL_OUTPUT,
        .channel_index = 1,
        .gpio = GPIO_NUM_4,
        .default_name = "Válvula de Dreno",
        .available = true,
        .role = "drain_valve"
    },
    {
        .plc_code = "DO02",
        .channel_class = DEVICE_CHANNEL_CLASS_DIGITAL_OUTPUT,
        .channel_index = 2,
        .gpio = GPIO_NUM_5,
        .default_name = "Válvula de Alimentação",
        .available = true,
        .role = "feed_valve"
    },
    {
        .plc_code = "DO03",
        .channel_class = DEVICE_CHANNEL_CLASS_DIGITAL_OUTPUT,
        .channel_index = 3,
        .gpio = GPIO_NUM_14,
        .default_name = "Sinalizador de Operação",
        .available = true,
        .role = "running"
    },
    {
        .plc_code = "DO04",
        .channel_class = DEVICE_CHANNEL_CLASS_DIGITAL_OUTPUT,
        .channel_index = 4,
        .gpio = GPIO_NUM_25,
        .default_name = "Alarme Sonoro Local",
        .available = true,
        .role = "alarm"
    },
    {
        .plc_code = "DO05",
        .channel_class = DEVICE_CHANNEL_CLASS_DIGITAL_OUTPUT,
        .channel_index = 5,
        .gpio = GPIO_NUM_33,
        .default_name = "Saída Digital 05",
        .available = true,
        .role = "output"
    },
    {
        .plc_code = "DO06",
        .channel_class = DEVICE_CHANNEL_CLASS_DIGITAL_OUTPUT,
        .channel_index = 6,
        .gpio = GPIO_NUM_NC,
        .default_name = "Válvula de Recirculação",
        .available = false,
        .role = "recirc_valve"
    },
    {
        .plc_code = "DO07",
        .channel_class = DEVICE_CHANNEL_CLASS_DIGITAL_OUTPUT,
        .channel_index = 7,
        .gpio = GPIO_NUM_NC,
        .default_name = "Aquecedor",
        .available = false,
        .role = "heater"
    },
    {
        .plc_code = "DO08",
        .channel_class = DEVICE_CHANNEL_CLASS_DIGITAL_OUTPUT,
        .channel_index = 8,
        .gpio = GPIO_NUM_NC,
        .default_name = "Válvula de Purga",
        .available = false,
        .role = "purge_valve"
    },
    {
        .plc_code = "DO09",
        .channel_class = DEVICE_CHANNEL_CLASS_DIGITAL_OUTPUT,
        .channel_index = 9,
        .gpio = GPIO_NUM_NC,
        .default_name = "Sinalizador Remoto de Interlock",
        .available = false,
        .role = "remote_signal"
    },

    /* --- ANALOG INPUTS (AI00..AI03) --- */
    {
        .plc_code = "AI00",
        .channel_class = DEVICE_CHANNEL_CLASS_ANALOG_INPUT,
        .channel_index = 0,
        .gpio = GPIO_NUM_34,
        .default_name = "Transdutor de Pressão",
        .available = true,
        .role = "pressure_sensor"
    },
    {
        .plc_code = "AI01",
        .channel_class = DEVICE_CHANNEL_CLASS_ANALOG_INPUT,
        .channel_index = 1,
        .gpio = GPIO_NUM_35,
        .default_name = "Sensor de Nível Analógico",
        .available = true,
        .role = "level_sensor"
    },
    {
        .plc_code = "AI02",
        .channel_class = DEVICE_CHANNEL_CLASS_ANALOG_INPUT,
        .channel_index = 2,
        .gpio = GPIO_NUM_36,
        .default_name = "Sensor de Temperatura",
        .available = true,
        .role = "temperature_sensor"
    },
    {
        .plc_code = "AI03",
        .channel_class = DEVICE_CHANNEL_CLASS_ANALOG_INPUT,
        .channel_index = 3,
        .gpio = GPIO_NUM_39,
        .default_name = "Sensor de Vazão",
        .available = true,
        .role = "flow_sensor"
    },

    /* --- ANALOG OUTPUT (AO00 - FUTURE) --- */
    {
        .plc_code = "AO00",
        .channel_class = DEVICE_CHANNEL_CLASS_ANALOG_OUTPUT,
        .channel_index = 0,
        .gpio = GPIO_NUM_NC,
        .default_name = "Referência VFD",
        .available = false,
        .role = "vfd_ref"
    },
};

static const plc_channel_desc_t relay_plc_channels[] = {
    {
        .plc_code = "DI00",
        .channel_class = DEVICE_CHANNEL_CLASS_DIGITAL_INPUT,
        .channel_index = 0,
        .gpio = GPIO_NUM_18,
        .default_name = "Entrada Digital 1",
        .available = true,
        .role = "input"
    },
    {
        .plc_code = "DI01",
        .channel_class = DEVICE_CHANNEL_CLASS_DIGITAL_INPUT,
        .channel_index = 1,
        .gpio = GPIO_NUM_19,
        .default_name = "Entrada Digital 2",
        .available = true,
        .role = "input"
    },
    {
        .plc_code = "DO00",
        .channel_class = DEVICE_CHANNEL_CLASS_DIGITAL_OUTPUT,
        .channel_index = 0,
        .gpio = GPIO_NUM_22,
        .default_name = "Relé 1",
        .available = true,
        .role = "relay"
    },
    {
        .plc_code = "DO01",
        .channel_class = DEVICE_CHANNEL_CLASS_DIGITAL_OUTPUT,
        .channel_index = 1,
        .gpio = GPIO_NUM_4,
        .default_name = "Relé 2",
        .available = true,
        .role = "relay"
    },
    {
        .plc_code = "AI00",
        .channel_class = DEVICE_CHANNEL_CLASS_ANALOG_INPUT,
        .channel_index = 0,
        .gpio = GPIO_NUM_NC,
        .default_name = "Entrada Analógica (Desabilitada)",
        .available = false,
        .role = "disabled"
    },
    {
        .plc_code = "AO00",
        .channel_class = DEVICE_CHANNEL_CLASS_ANALOG_OUTPUT,
        .channel_index = 0,
        .gpio = GPIO_NUM_NC,
        .default_name = "Saída Analógica (Desabilitada)",
        .available = false,
        .role = "disabled"
    },
};

static const plc_channel_desc_t sensor_plc_channels[] = {
    {
        .plc_code = "DI00",
        .channel_class = DEVICE_CHANNEL_CLASS_DIGITAL_INPUT,
        .channel_index = 0,
        .gpio = GPIO_NUM_16,
        .default_name = "Sensor Digital 1",
        .available = true,
        .role = "sensor"
    },
    {
        .plc_code = "DI01",
        .channel_class = DEVICE_CHANNEL_CLASS_DIGITAL_INPUT,
        .channel_index = 1,
        .gpio = GPIO_NUM_17,
        .default_name = "Sensor Digital 2",
        .available = true,
        .role = "sensor"
    },
    {
        .plc_code = "DO00",
        .channel_class = DEVICE_CHANNEL_CLASS_DIGITAL_OUTPUT,
        .channel_index = 0,
        .gpio = GPIO_NUM_22,
        .default_name = "Status LED / Atuador",
        .available = true,
        .role = "status"
    },
    {
        .plc_code = "DO01",
        .channel_class = DEVICE_CHANNEL_CLASS_DIGITAL_OUTPUT,
        .channel_index = 1,
        .gpio = GPIO_NUM_4,
        .default_name = "Alarme Local",
        .available = true,
        .role = "alarm"
    },
    {
        .plc_code = "AI00",
        .channel_class = DEVICE_CHANNEL_CLASS_ANALOG_INPUT,
        .channel_index = 0,
        .gpio = GPIO_NUM_34,
        .default_name = "Sensor Analógico 1",
        .available = true,
        .role = "analog_sensor"
    },
    {
        .plc_code = "AO00",
        .channel_class = DEVICE_CHANNEL_CLASS_ANALOG_OUTPUT,
        .channel_index = 0,
        .gpio = GPIO_NUM_NC,
        .default_name = "Saída Analógica (Desabilitada)",
        .available = false,
        .role = "disabled"
    },
};

static const node_profile_desc_t gateway_profile = {
    .type = NODE_PROFILE_GATEWAY,
    .label = "Gateway",
    .node_caps = &gateway_node_caps,
    .exp_caps = &gateway_exp_caps,
    .network = &gateway_net_profile,
    .chan_groups = NULL,
    .chan_groups_len = 0,
    .plc_channels = gateway_plc_channels,
    .plc_channels_len = ARRAY_LEN(gateway_plc_channels),
};

/* -------------------------------------------------------------------- */
/*  FIELD NODE PROFILE                                                  */
/* -------------------------------------------------------------------- */
static const device_node_capabilities_t field_node_caps = {
    .local_input_slot_capacity = 16,
    .local_output_slot_capacity = 16,
    .default_input_count = 6,
    .default_output_count = 6,
    .distributed_scaling = true,
    .supports_remote_nodes = false,
    .supports_mcp_digital = true,
    .supports_native_analog = true,
    .supports_external_analog = true,
    .global_capacity_mode = "field-executor",
    .local_capacity_mode = "field-io",
    .recommended_scaling_path = "No de campo focado em I/O local digital e analogico.",
};

static const device_expansion_capabilities_t field_exp_caps = {
    .supports_mcp23x17 = true,
    .recommended_mcp_instances = 2,
    .channels_per_mcp = 16,
    .supports_ads1115 = true,
    .recommended_external_adc_instances = 1,
    .channels_per_external_adc = 4,
    .native_analog_input_channels = 4,
    .notes = "Suporte completo a expansores de I/O de campo.",
};

static const device_network_profile_t field_net_profile = {
    .wifi_supported = true,
    .ethernet_supported = true,
    .rs485_supported = CONFIG_ENDAP_RS485_ENABLED,
    .wifi_enabled = true,
    .ethernet_enabled = false,
    .rs485_enabled = CONFIG_ENDAP_RS485_ENABLED,
    .wifi_mode = DEVICE_PROFILE_WIFI_MODE_INFRA,
    .onboarding_pending = true,
    .primary_transport = CONFIG_ENDAP_RS485_ENABLED ? DEVICE_PROFILE_TRANSPORT_RS485 : DEVICE_PROFILE_TRANSPORT_WIFI,
    .fallback_transport = CONFIG_ENDAP_RS485_ENABLED ? DEVICE_PROFILE_TRANSPORT_WIFI : DEVICE_PROFILE_TRANSPORT_NONE,
    .failover_delay_ms = 5000,
    .recovery_hysteresis_ms = 15000,
    .ethernet_mode = DEVICE_PROFILE_ETH_NONE,
    .allow_local_ap = false,
    .allow_dashboard = true,
    .allow_ota = true,
    .label = "field-network",
};

static const node_profile_desc_t field_profile = {
    .type = NODE_PROFILE_FIELD,
    .label = "Field Node",
    .node_caps = &field_node_caps,
    .exp_caps = &field_exp_caps,
    .network = &field_net_profile,
    .chan_groups = NULL,
    .chan_groups_len = 0,
    .plc_channels = field_plc_channels,
    .plc_channels_len = ARRAY_LEN(field_plc_channels),
};

/* -------------------------------------------------------------------- */
/*  RELAY NODE PROFILE                                                  */
/* -------------------------------------------------------------------- */
static const device_node_capabilities_t relay_node_caps = {
    .local_input_slot_capacity = 4,
    .local_output_slot_capacity = 16,
    .default_input_count = 2,
    .default_output_count = 16,
    .distributed_scaling = false,
    .supports_remote_nodes = false,
    .supports_mcp_digital = true,
    .supports_native_analog = false,
    .supports_external_analog = false,
    .global_capacity_mode = "relay-actuator",
    .local_capacity_mode = "output-dense",
    .recommended_scaling_path = "Especializado em atuacao e reles.",
};

static const device_expansion_capabilities_t relay_exp_caps = {
    .supports_mcp23x17 = true,
    .recommended_mcp_instances = 2,
    .channels_per_mcp = 16,
    .supports_ads1115 = false,
    .recommended_external_adc_instances = 0,
    .channels_per_external_adc = 0,
    .native_analog_input_channels = 0,
    .notes = "Expansao otimizada para reles (MCP23x17).",
};

static const device_network_profile_t relay_net_profile = {
    .wifi_supported = true,
    .ethernet_supported = false,
    .rs485_supported = CONFIG_ENDAP_RS485_ENABLED,
    .wifi_enabled = true,
    .ethernet_enabled = false,
    .rs485_enabled = CONFIG_ENDAP_RS485_ENABLED,
    .wifi_mode = DEVICE_PROFILE_WIFI_MODE_INFRA,
    .onboarding_pending = true,
    .primary_transport = CONFIG_ENDAP_RS485_ENABLED ? DEVICE_PROFILE_TRANSPORT_RS485 : DEVICE_PROFILE_TRANSPORT_WIFI,
    .fallback_transport = DEVICE_PROFILE_TRANSPORT_NONE,
    .failover_delay_ms = 5000,
    .recovery_hysteresis_ms = 15000,
    .ethernet_mode = DEVICE_PROFILE_ETH_NONE,
    .allow_local_ap = false,
    .allow_dashboard = false,
    .allow_ota = true,
    .label = "relay-network",
};

static const node_profile_desc_t relay_profile = {
    .type = NODE_PROFILE_RELAY,
    .label = "Relay Node",
    .node_caps = &relay_node_caps,
    .exp_caps = &relay_exp_caps,
    .network = &relay_net_profile,
    .chan_groups = NULL,
    .chan_groups_len = 0,
    .plc_channels = relay_plc_channels,
    .plc_channels_len = ARRAY_LEN(relay_plc_channels),
};

/* -------------------------------------------------------------------- */
/*  SENSOR NODE PROFILE                                                 */
/* -------------------------------------------------------------------- */
static const device_node_capabilities_t sensor_node_caps = {
    .local_input_slot_capacity = 16,
    .local_output_slot_capacity = 2,
    .default_input_count = 16,
    .default_output_count = 0,
    .distributed_scaling = false,
    .supports_remote_nodes = false,
    .supports_mcp_digital = true,
    .supports_native_analog = true,
    .supports_external_analog = true,
    .global_capacity_mode = "sensor-collector",
    .local_capacity_mode = "input-dense",
    .recommended_scaling_path = "Especializado em aquisicao de sensores digitais e analogicos.",
};

static const device_expansion_capabilities_t sensor_exp_caps = {
    .supports_mcp23x17 = true,
    .recommended_mcp_instances = 1,
    .channels_per_mcp = 16,
    .supports_ads1115 = true,
    .recommended_external_adc_instances = 2,
    .channels_per_external_adc = 4,
    .native_analog_input_channels = 6,
    .notes = "Expansao otimizada para aquisicao analogica (ADS1115) e entradas.",
};

static const device_network_profile_t sensor_net_profile = {
    .wifi_supported = true,
    .ethernet_supported = false,
    .rs485_supported = CONFIG_ENDAP_RS485_ENABLED,
    .wifi_enabled = true,
    .ethernet_enabled = false,
    .rs485_enabled = CONFIG_ENDAP_RS485_ENABLED,
    .wifi_mode = DEVICE_PROFILE_WIFI_MODE_INFRA,
    .onboarding_pending = true,
    .primary_transport = DEVICE_PROFILE_TRANSPORT_WIFI,
    .fallback_transport = CONFIG_ENDAP_RS485_ENABLED ? DEVICE_PROFILE_TRANSPORT_RS485 : DEVICE_PROFILE_TRANSPORT_NONE,
    .failover_delay_ms = 5000,
    .recovery_hysteresis_ms = 15000,
    .ethernet_mode = DEVICE_PROFILE_ETH_NONE,
    .allow_local_ap = false,
    .allow_dashboard = false,
    .allow_ota = true,
    .label = "sensor-network",
};

static const node_profile_desc_t sensor_profile = {
    .type = NODE_PROFILE_SENSOR,
    .label = "Sensor Node",
    .node_caps = &sensor_node_caps,
    .exp_caps = &sensor_exp_caps,
    .network = &sensor_net_profile,
    .chan_groups = NULL,
    .chan_groups_len = 0,
    .plc_channels = sensor_plc_channels,
    .plc_channels_len = ARRAY_LEN(sensor_plc_channels),
};

/* -------------------------------------------------------------------- */
/*  CUSTOM PROFILE (Fallback)                                          */
/* -------------------------------------------------------------------- */
static const node_profile_desc_t custom_profile = {
    .type = NODE_PROFILE_CUSTOM,
    .label = "Custom Node",
    .node_caps = &field_node_caps,
    .exp_caps = &field_exp_caps,
    .network = &field_net_profile,
    .chan_groups = NULL,
    .chan_groups_len = 0,
    .plc_channels = field_plc_channels,
    .plc_channels_len = ARRAY_LEN(field_plc_channels),
};

/* Tabela de templates indexada por node_profile_t */
const node_profile_desc_t * const node_profile_templates[NODE_PROFILE_MAX] = {
    [NODE_PROFILE_GATEWAY] = &gateway_profile,
    [NODE_PROFILE_FIELD]   = &field_profile,
    [NODE_PROFILE_RELAY]   = &relay_profile,
    [NODE_PROFILE_SENSOR]  = &sensor_profile,
    [NODE_PROFILE_CUSTOM]  = &custom_profile,
};

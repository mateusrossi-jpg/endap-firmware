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
    .rs485_supported = true,
    .wifi_enabled = true,
    .ethernet_enabled = false,
    .rs485_enabled = true,
    .wifi_mode = DEVICE_PROFILE_WIFI_MODE_INFRA,
    .onboarding_pending = false,
    .primary_transport = DEVICE_PROFILE_TRANSPORT_WIFI,
    .fallback_transport = DEVICE_PROFILE_TRANSPORT_RS485,
    .failover_delay_ms = 5000,
    .recovery_hysteresis_ms = 15000,
    .ethernet_mode = DEVICE_PROFILE_ETH_NONE,
    .allow_local_ap = true,
    .allow_dashboard = true,
    .allow_ota = true,
    .label = "gateway-network",
};

static const node_profile_desc_t gateway_profile = {
    .type = NODE_PROFILE_GATEWAY,
    .label = "Gateway",
    .node_caps = &gateway_node_caps,
    .exp_caps = &gateway_exp_caps,
    .network = &gateway_net_profile,
    // TODO(v1.1): Preencher channel inventory groups por perfil quando o Dashboard e Onboarding precisarem listar canais.
    .chan_groups = NULL,
    .chan_groups_len = 0,
};

/* -------------------------------------------------------------------- */
/*  FIELD NODE PROFILE                                                  */
/* -------------------------------------------------------------------- */
static const device_node_capabilities_t field_node_caps = {
    .local_input_slot_capacity = 16,
    .local_output_slot_capacity = 16,
    .default_input_count = 8,
    .default_output_count = 8,
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
    .native_analog_input_channels = 6,
    .notes = "Suporte completo a expansores de I/O de campo.",
};

static const device_network_profile_t field_net_profile = {
    .wifi_supported = true,
    .ethernet_supported = true,
    .rs485_supported = true,
    .wifi_enabled = true,
    .ethernet_enabled = false,
    .rs485_enabled = true,
    .wifi_mode = DEVICE_PROFILE_WIFI_MODE_INFRA,
    .onboarding_pending = true,
    .primary_transport = DEVICE_PROFILE_TRANSPORT_RS485,
    .fallback_transport = DEVICE_PROFILE_TRANSPORT_WIFI,
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
    .rs485_supported = true,
    .wifi_enabled = false,
    .ethernet_enabled = false,
    .rs485_enabled = true,
    .wifi_mode = DEVICE_PROFILE_WIFI_MODE_INFRA,
    .onboarding_pending = true,
    .primary_transport = DEVICE_PROFILE_TRANSPORT_RS485,
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
    .rs485_supported = true,
    .wifi_enabled = true,
    .ethernet_enabled = false,
    .rs485_enabled = true,
    .wifi_mode = DEVICE_PROFILE_WIFI_MODE_INFRA,
    .onboarding_pending = true,
    .primary_transport = DEVICE_PROFILE_TRANSPORT_WIFI,
    .fallback_transport = DEVICE_PROFILE_TRANSPORT_RS485,
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
};

/* Tabela de templates indexada por node_profile_t */
const node_profile_desc_t * const node_profile_templates[NODE_PROFILE_MAX] = {
    [NODE_PROFILE_GATEWAY] = &gateway_profile,
    [NODE_PROFILE_FIELD]   = &field_profile,
    [NODE_PROFILE_RELAY]   = &relay_profile,
    [NODE_PROFILE_SENSOR]  = &sensor_profile,
    [NODE_PROFILE_CUSTOM]  = &custom_profile,
};

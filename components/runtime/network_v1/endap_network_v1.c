#include "endap_network_v1.h"
#include "cluster_transport.h"
#include "cluster_failover.h"
#include "node_registry.h"
#include "node_identity.h"
#include "device_profile.h"
#include "esp_log.h"
#include "esp_timer.h"

#include <string.h>

#define TAG "NET_V1"

/* Estado interno mantido em buffer estático */
static endap_network_state_t g_net_state = ENDAP_NET_STATE_DISCONNECTED;
static endap_network_metrics_t g_net_metrics = {0};

static uint32_t g_last_primary_ok_ms = 0;
static uint32_t g_degraded_start_ms = 0;
static uint32_t g_recovering_start_ms = 0;
static bool g_net_v1_initialized = false;

/* Configuração em cache do perfil ativo */
static device_profile_transport_t g_primary_transport = DEVICE_PROFILE_TRANSPORT_WIFI;
static device_profile_transport_t g_fallback_transport = DEVICE_PROFILE_TRANSPORT_RS485;
static uint32_t g_failover_delay_ms = 5000;
static uint32_t g_recovery_hysteresis_ms = 15000;
static bool g_is_gateway = false;

static uint32_t now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000ULL);
}

static cluster_transport_type_t map_profile_transport_to_cluster(device_profile_transport_t t)
{
    switch (t)
    {
        case DEVICE_PROFILE_TRANSPORT_WIFI:
            return CLUSTER_TRANSPORT_WIFI_UDP;
        case DEVICE_PROFILE_TRANSPORT_ETHERNET:
            return CLUSTER_TRANSPORT_ETHERNET_UDP;
        case DEVICE_PROFILE_TRANSPORT_RS485:
            return CLUSTER_TRANSPORT_RS485;
        default:
            return CLUSTER_TRANSPORT_NONE;
    }
}

esp_err_t endap_network_v1_init(void)
{
    if (g_net_v1_initialized)
    {
        return ESP_OK;
    }

    const node_profile_desc_t *curr_prof = device_profile_get_current();
    if (curr_prof && curr_prof->network)
    {
        g_primary_transport = curr_prof->network->primary_transport;
        g_fallback_transport = curr_prof->network->fallback_transport;
        g_failover_delay_ms = curr_prof->network->failover_delay_ms;
        g_recovery_hysteresis_ms = curr_prof->network->recovery_hysteresis_ms;
        g_is_gateway = (curr_prof->type == NODE_PROFILE_GATEWAY);
    }

    if (g_failover_delay_ms == 0) g_failover_delay_ms = 5000;
    if (g_recovery_hysteresis_ms == 0) g_recovery_hysteresis_ms = 15000;

    memset(&g_net_metrics, 0, sizeof(g_net_metrics));
    cluster_transport_type_t initial_transport = map_profile_transport_to_cluster(g_primary_transport);
    g_net_metrics.active_transport = (uint8_t)initial_transport;
    g_net_metrics.current_state = (uint8_t)ENDAP_NET_STATE_CONNECTING;

    g_net_state = ENDAP_NET_STATE_CONNECTING;
    g_last_primary_ok_ms = now_ms();

    uint32_t self_id = node_identity_get();
    cluster_failover_init(self_id);

    ESP_LOGI(TAG, "Network v1 inicializada: is_gateway=%d, primary=%s, fallback=%s, failover_delay=%" PRIu32 "ms, hysteresis=%" PRIu32 "ms",
             g_is_gateway,
             cluster_transport_name(initial_transport),
             cluster_transport_name(map_profile_transport_to_cluster(g_fallback_transport)),
             g_failover_delay_ms,
             g_recovery_hysteresis_ms);

    g_net_v1_initialized = true;
    return ESP_OK;
}

void endap_network_v1_process(void)
{
    if (!g_net_v1_initialized)
    {
        endap_network_v1_init();
    }

    uint32_t current_time = now_ms();
    bool transport_ready = cluster_transport_is_ready();

    // Atualiza estado de transporte no node_registry para o nó local
    uint32_t self_id = node_identity_get();
    node_registry_note_transport(self_id, g_net_metrics.active_transport, 0);

    switch (g_net_state)
    {
        case ENDAP_NET_STATE_DISCONNECTED:
            if (transport_ready)
            {
                g_net_state = ENDAP_NET_STATE_CONNECTING;
                ESP_LOGI(TAG, "Transporte pronto, transitando para CONNECTING");
            }
            break;

        case ENDAP_NET_STATE_CONNECTING:
            if (transport_ready)
            {
                g_net_state = ENDAP_NET_STATE_CONNECTED;
                g_last_primary_ok_ms = current_time;
                ESP_LOGI(TAG, "Conexao estabelecida no transporte primario (%s)",
                         cluster_transport_name(map_profile_transport_to_cluster(g_primary_transport)));
            }
            break;

        case ENDAP_NET_STATE_CONNECTED:
        {
            uint32_t quiet_time = current_time - g_last_primary_ok_ms;
            
            // Gateway nao aplica failover se um no remetente sumir, mas Field Nodes monitoram link primario
            if (!g_is_gateway && quiet_time > g_failover_delay_ms && g_fallback_transport != DEVICE_PROFILE_TRANSPORT_NONE)
            {
                cluster_transport_type_t fb_transport = map_profile_transport_to_cluster(g_fallback_transport);
                ESP_LOGW(TAG, "Timeout no transporte primario (quiet=%" PRIu32 "ms). Transicionando para DEGRADED (%s)...",
                         quiet_time, cluster_transport_name(fb_transport));

                g_net_state = ENDAP_NET_STATE_DEGRADED;
                g_degraded_start_ms = current_time;
                g_net_metrics.failover_count++;
                g_net_metrics.active_transport = (uint8_t)fb_transport;
                cluster_transport_set_active_type(fb_transport);
            }
            break;
        }

        case ENDAP_NET_STATE_DEGRADED:
        {
            uint32_t degraded_duration = current_time - g_degraded_start_ms;
            // Tenta testar recuperacao apos o tempo de hysteresis acumulado
            if (degraded_duration >= g_recovery_hysteresis_ms)
            {
                cluster_transport_type_t prim_transport = map_profile_transport_to_cluster(g_primary_transport);
                ESP_LOGI(TAG, "Hysteresis concluida. Tentando RECOVERING para o transporte primario (%s)...",
                         cluster_transport_name(prim_transport));

                g_net_state = ENDAP_NET_STATE_RECOVERING;
                g_recovering_start_ms = current_time;
                g_net_metrics.active_transport = (uint8_t)prim_transport;
                cluster_transport_set_active_type(prim_transport);
            }
            break;
        }

        case ENDAP_NET_STATE_RECOVERING:
        {
            uint32_t recovering_time = current_time - g_recovering_start_ms;
            if (transport_ready && (current_time - g_last_primary_ok_ms < g_failover_delay_ms))
            {
                ESP_LOGI(TAG, "Recuperacao bem-sucedida! Retornando ao estado CONNECTED.");
                g_net_state = ENDAP_NET_STATE_CONNECTED;
            }
            else if (recovering_time > (g_failover_delay_ms / 2))
            {
                cluster_transport_type_t fb_transport = map_profile_transport_to_cluster(g_fallback_transport);
                ESP_LOGW(TAG, "Falha na recuperacao do primario. Retornando ao transporte DEGRADED (%s).",
                         cluster_transport_name(fb_transport));

                g_net_state = ENDAP_NET_STATE_DEGRADED;
                g_degraded_start_ms = current_time;
                g_net_metrics.active_transport = (uint8_t)fb_transport;
                cluster_transport_set_active_type(fb_transport);
            }
            break;
        }

        default:
            break;
    }

    g_net_metrics.current_state = (uint8_t)g_net_state;
}

endap_network_state_t endap_network_v1_get_state(void)
{
    return g_net_state;
}

void endap_network_v1_note_packet(bool is_rx, bool success, uint32_t rtt_ms)
{
    // Métricas leves atômicas sem locks pesados
    if (is_rx)
    {
        g_net_metrics.packets_received++;
        g_last_primary_ok_ms = now_ms();
    }
    else
    {
        g_net_metrics.packets_sent++;
        if (!success)
        {
            g_net_metrics.packet_drops++;
        }
    }

    if (rtt_ms > 0)
    {
        g_net_metrics.last_rtt_ms = rtt_ms;
        if (rtt_ms > g_net_metrics.max_jitter_ms)
        {
            g_net_metrics.max_jitter_ms = rtt_ms;
        }
    }
}

void endap_network_v1_get_metrics(endap_network_metrics_t *out_metrics)
{
    if (!out_metrics) return;
    *out_metrics = g_net_metrics;
}

esp_err_t endap_network_v1_force_reconnect(void)
{
    ESP_LOGW(TAG, "Forcando reconexao manual da rede...");
    g_net_state = ENDAP_NET_STATE_CONNECTING;
    g_last_primary_ok_ms = now_ms();
    cluster_transport_type_t prim_transport = map_profile_transport_to_cluster(g_primary_transport);
    g_net_metrics.active_transport = (uint8_t)prim_transport;
    cluster_transport_set_active_type(prim_transport);
    return ESP_OK;
}

const char *endap_network_v1_state_name(endap_network_state_t state)
{
    switch (state)
    {
        case ENDAP_NET_STATE_DISCONNECTED:
            return "DISCONNECTED";
        case ENDAP_NET_STATE_CONNECTING:
            return "CONNECTING";
        case ENDAP_NET_STATE_CONNECTED:
            return "CONNECTED";
        case ENDAP_NET_STATE_DEGRADED:
            return "DEGRADED";
        case ENDAP_NET_STATE_RECOVERING:
            return "RECOVERING";
        default:
            return "UNKNOWN";
    }
}

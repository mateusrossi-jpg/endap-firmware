#include "cluster_failover.h"
#include "cluster_io.h"
#include "cluster_events.h"
#include "failsafe.h"
#include "node_registry.h"

#include "esp_log.h"
#include <stdbool.h>
#include <string.h>
#include <inttypes.h>

static const char *TAG = "CLUSTER_FAILOVER";

static uint32_t self_node = 0;
static bool failover_started = false;

static bool cluster_failover_node_is_registered_gateway(uint32_t node_id)
{
    node_registry_entry_t snapshot[NODE_REGISTRY_MAX_NODES];
    int count = node_registry_export(snapshot, NODE_REGISTRY_MAX_NODES);

    for (int i = 0; i < count; i++)
    {
        if (snapshot[i].node_id == node_id &&
            strcmp(snapshot[i].profile, "gateway") == 0)
        {
            return true;
        }
    }

    return false;
}

static void cluster_failover_apply_local_failsafe_if_gateway_lost(uint32_t node_id,
                                                                  failsafe_reason_t reason)
{
    int applied;

    if (node_id == self_node)
        return;

    /*
     * Fail-safe por perda de mestre deve proteger principalmente nos de campo.
     * O gateway nao deve derrubar suas proprias saidas quando um relay/sensor some.
     * Por isso usamos o perfil administrativo salvo no registry como filtro minimo.
     */
    if (!cluster_failover_node_is_registered_gateway(node_id))
        return;

    applied = failsafe_apply_for_reason_all(reason);
    if (applied > 0)
    {
        ESP_LOGW(TAG,
                 "FAILSAFE: gateway %" PRIu32 " perdido -> %d saida(s) locais em estado seguro",
                 node_id,
                 applied);
    }
}

/* ============================================================
   EVENT HANDLER
============================================================ */

static void cluster_failover_event_handler(cluster_event_t *evt)
{
    if (!evt) return;

    switch (evt->type)
    {
        case EVENT_NODE_OFFLINE:
        {
            if (evt->node_id == self_node)
                return;

            ESP_LOGW(TAG,
                "FAILOVER: node %" PRIu32 " OFFLINE",
                evt->node_id);

            cluster_failover_apply_local_failsafe_if_gateway_lost(evt->node_id,
                                                                  FAILSAFE_REASON_NODE_OFFLINE);
            cluster_io_handle_node_offline(evt->node_id);
            break;
        }

        case EVENT_NODE_ONLINE:
        {
            if (evt->node_id == self_node)
                return;

            ESP_LOGI(TAG,
                "FAILBACK: node %" PRIu32 " ONLINE",
                evt->node_id);

            cluster_io_handle_node_online(evt->node_id);
            break;
        }

        case EVENT_NODE_SUSPECT:
        {
            ESP_LOGW(TAG,
                "Node %" PRIu32 " SUSPECT",
                evt->node_id);
            cluster_failover_apply_local_failsafe_if_gateway_lost(evt->node_id,
                                                                  FAILSAFE_REASON_COMM_LOSS);
            break;
        }

        default:
            break;
    }
}

/* ============================================================
   INIT
============================================================ */

void cluster_failover_init(uint32_t self_node_id)
{
    if (failover_started)
    {
        self_node = self_node_id;
        return;
    }

    self_node = self_node_id;

    cluster_io_init(self_node);

    cluster_events_subscribe(cluster_failover_event_handler);
    failover_started = true;

    ESP_LOGI(TAG,
        "Failover engine iniciado (node=%" PRIu32 ")",
        self_node);
}

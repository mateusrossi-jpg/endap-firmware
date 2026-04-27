#include "cluster_discovery.h"

#include "cluster_manager.h"
#include "cluster_transport.h"
#include "node_registry.h"
#include "node_identity.h"

#include "esp_log.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <inttypes.h>
#include <stdbool.h>

static const char *TAG = "CLUSTER_DISC";

static uint32_t self_node_id = 0;
static bool discovery_started = false;
static TaskHandle_t discovery_task_handle = NULL;

static void cluster_discovery_on_heartbeat(const cluster_transport_heartbeat_t *heartbeat)
{
    node_registry_state_t registry_state;

    if (!heartbeat || heartbeat->node_id == self_node_id)
        return;

    node_registry_note_transport(heartbeat->node_id,
                                 heartbeat->source_transport,
                                 heartbeat->source_ip);
    registry_state = node_registry_get_state(heartbeat->node_id);

    if (node_registry_is_operational(heartbeat->node_id))
    {
        cluster_manager_update_node(heartbeat->node_id, heartbeat->source_ip);

        ESP_LOGI(TAG,
                 "Node operacional detectado: %" PRIu32 " via %s",
                 heartbeat->node_id,
                 cluster_transport_name((cluster_transport_type_t)heartbeat->source_transport));
        return;
    }

    cluster_manager_remove_node(heartbeat->node_id);

    ESP_LOGI(TAG,
             "Node detectado aguardando admissao: %" PRIu32 " via %s (state=%s)",
             heartbeat->node_id,
             cluster_transport_name((cluster_transport_type_t)heartbeat->source_transport),
             node_registry_state_name(registry_state));
}

static void discovery_broadcast_task(void *arg)
{
    (void)arg;

    /* atraso inicial para o transporte e a rede estabilizarem */
    vTaskDelay(pdMS_TO_TICKS(1000));

    while (1)
    {
        if (cluster_transport_is_ready())
            cluster_transport_send_heartbeat();

        vTaskDelay(pdMS_TO_TICKS(DISCOVERY_INTERVAL_MS));
    }
}

void cluster_discovery_start(void)
{
    BaseType_t ok;

    if (discovery_started)
        return;

    self_node_id = node_identity_get();
    cluster_transport_register_heartbeat_callback(cluster_discovery_on_heartbeat);

    ok = xTaskCreatePinnedToCore(
        discovery_broadcast_task,
        "disc_tx",
        4096,
        NULL,
        4,
        &discovery_task_handle,
        0);

    if (ok != pdPASS)
    {
        discovery_task_handle = NULL;
        ESP_LOGE(TAG, "Falha ao criar task de discovery");
        return;
    }

    discovery_started = true;
    ESP_LOGI(TAG, "Cluster discovery iniciado (%s)", cluster_transport_active_name());
}

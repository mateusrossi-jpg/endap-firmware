#include "cluster_discovery.h"

#include "cluster_manager.h"
#include "cluster_transport.h"
#include "node_registry.h"
#include "node_identity.h"

#include "esp_log.h"
#include "esp_random.h"
#include "esp_heap_caps.h"

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
    if (!heartbeat || heartbeat->node_id == self_node_id)
        return;

    cluster_manager_update_node(heartbeat);
    node_registry_note_transport(heartbeat->node_id, heartbeat->source_transport, heartbeat->source_ip);

    ESP_LOGI(TAG,
             "Node detectado via %s: %" PRIu32,
             cluster_transport_name((cluster_transport_type_t)heartbeat->source_transport),
             heartbeat->node_id);
}

static void discovery_broadcast_task(void *arg)
{
    (void)arg;

    /* jitter (500ms a 5000ms) */
    vTaskDelay(pdMS_TO_TICKS(500 + (esp_random() % 4501)));

    while (1)
    {
        if (cluster_transport_is_ready())
            cluster_transport_send_heartbeat();

        /* A cada 2500 ms Node envia heartbeat */
        vTaskDelay(pdMS_TO_TICKS(2500));
    }
}

void cluster_discovery_start(void)
{
    BaseType_t ok;

    if (discovery_started)
        return;

    self_node_id = node_identity_get();
    cluster_transport_register_heartbeat_callback(cluster_discovery_on_heartbeat);

    ESP_LOGI(TAG, "Heap before task creation: free=%u largest=%u",
             (unsigned int)heap_caps_get_free_size(MALLOC_CAP_8BIT),
             (unsigned int)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));

    ok = xTaskCreatePinnedToCore(
        discovery_broadcast_task,
        "disc_tx",
        2048,
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

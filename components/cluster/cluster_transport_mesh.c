#include "cluster_transport_mesh.h"
#include "esp_mesh.h"
#include "esp_wifi.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include <string.h>

static const char *TAG = "CLUSTER_MESH";
static uint32_t current_node_id = 0;
static cluster_transport_heartbeat_cb_t mesh_heartbeat_cb = NULL;
static cluster_transport_frame_cb_t mesh_frame_cb = NULL;
static bool mesh_initialized = false;
static volatile bool rx_task_running = false;
static TaskHandle_t volatile mesh_rx_task_handle = NULL;
static SemaphoreHandle_t mesh_rx_stop_sem = NULL;

static void cluster_mesh_rx_task(void *arg)
{
    esp_err_t err;
    mesh_addr_t from;
    mesh_data_t data;
    int flag = 0;
    
    // Allocate rx buffer
    data.data = malloc(1500); // Typical MTU
    data.size = 1500;

    if (!data.data) {
        ESP_LOGE(TAG, "Falha ao alocar buffer para MESH RX");
        if (mesh_rx_stop_sem) xSemaphoreGive(mesh_rx_stop_sem);
        vTaskDelete(NULL);
        return;
    }

    rx_task_running = true;
    while (rx_task_running)
    {
        data.size = 1500;
        err = esp_mesh_recv(&from, &data, pdMS_TO_TICKS(500), &flag, NULL, 0);
        if (err == ESP_OK && data.size > 0 && data.data != NULL)
        {
            if (!rx_task_running) break;

            if (data.size >= sizeof(endap_heartbeat_t))
            {
                endap_heartbeat_t *hb = (endap_heartbeat_t *)data.data;
                if (hb->magic == 0x454E4450)
                {
                    if (mesh_heartbeat_cb)
                    {
                        uint32_t sender_id = hb->node_id;
                        if (sender_id != current_node_id)
                        {
                            cluster_transport_heartbeat_t heartbeat = {
                                .node_id = sender_id,
                                .timestamp_ms = hb->uptime_seconds * 1000U,
                                .source_ip = 0U,
                                .source_transport = (uint8_t)CLUSTER_TRANSPORT_WIFI_MESH,
                                .device_profile = hb->node_type,
                                .uptime_seconds = hb->uptime_seconds,
                                .flags = hb->state
                            };
                            mesh_heartbeat_cb(&heartbeat);
                        }
                    }
                    continue;
                }
            }

            if (mesh_frame_cb)
            {
                mesh_frame_cb(data.data, (uint16_t)data.size);
            }
        }
    }
    
    free(data.data);
    mesh_rx_task_handle = NULL;
    if (mesh_rx_stop_sem) {
        xSemaphoreGive(mesh_rx_stop_sem);
    }
    vTaskDelete(NULL);
}

bool cluster_transport_mesh_init(uint32_t self_node_id)
{
    if (mesh_initialized) return true;

    current_node_id = self_node_id;

    if (!mesh_rx_stop_sem) {
        mesh_rx_stop_sem = xSemaphoreCreateBinary();
    } else {
        xSemaphoreTake(mesh_rx_stop_sem, 0);
    }

    if (xTaskCreatePinnedToCore(cluster_mesh_rx_task, "cluster_mesh_rx", 4096, NULL, 5, (TaskHandle_t *)&mesh_rx_task_handle, 0) != pdPASS)
    {
        ESP_LOGE(TAG, "Falha ao criar task MESH RX");
        return false;
    }

    mesh_initialized = true;
    ESP_LOGI(TAG, "Transporte ESP-MESH inicializado");
    return true;
}

void cluster_transport_mesh_deinit(void)
{
    if (!mesh_initialized) return;
    
    rx_task_running = false;
    
    /* Envia unibytes de stop UNICAST para o próprio nó (Loopback Local) ANTES de zerar mesh_initialized */
    if (mesh_rx_task_handle != NULL)
    {
        mesh_addr_t self_mac = {0};
        esp_wifi_get_mac(WIFI_IF_STA, self_mac.addr);

        endap_heartbeat_t stop_hb = {
            .magic = 0x454E4450,
            .node_id = current_node_id,
            .node_type = 0,
            .state = 0,
            .uptime_seconds = 0
        };

        mesh_data_t mesh_data = {
            .data = (uint8_t *)&stop_hb,
            .size = sizeof(stop_hb),
            .proto = MESH_PROTO_BIN,
            .tos = MESH_TOS_P2P
        };

        // Envio Unicast exclusivo para o próprio nó
        esp_mesh_send(&self_mac, &mesh_data, MESH_DATA_P2P, NULL, 0);

        // Aguarda encerramento gracioso via Semáforo por até 1000ms
        if (mesh_rx_stop_sem && xSemaphoreTake(mesh_rx_stop_sem, pdMS_TO_TICKS(1000)) != pdTRUE)
        {
            ESP_LOGW(TAG, "Task MESH RX nao respondeu ao semaforo no tempo; forçando vTaskDelete");
            if (mesh_rx_task_handle) {
                vTaskDelete(mesh_rx_task_handle);
                mesh_rx_task_handle = NULL;
            }
        }
    }
    
    mesh_initialized = false;
    ESP_LOGI(TAG, "Transporte ESP-MESH desativado com sucesso (graceful shutdown)");
}

bool cluster_transport_mesh_send(const uint8_t *data, size_t len)
{
    if (!mesh_initialized || !data || len == 0) return false;

    // Send to broadcast MAC address
    mesh_addr_t to_mac = { .addr = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF } };
    mesh_data_t mesh_data = {
        .data = (uint8_t *)data,
        .size = len,
        .proto = MESH_PROTO_BIN,
        .tos = MESH_TOS_P2P
    };

    esp_err_t err = esp_mesh_send(&to_mac, &mesh_data, MESH_DATA_P2P, NULL, 0);
    if (err != ESP_OK)
    {
        ESP_LOGD(TAG, "Falha no envio ESP-MESH: %s", esp_err_to_name(err));
        return false;
    }

    return true;
}

void cluster_transport_mesh_register_callbacks(cluster_transport_heartbeat_cb_t hb_cb, cluster_transport_frame_cb_t frame_cb)
{
    mesh_heartbeat_cb = hb_cb;
    mesh_frame_cb = frame_cb;
}

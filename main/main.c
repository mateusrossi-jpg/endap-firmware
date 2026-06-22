#include "core_supervisor.h"

#include "esp_log.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <inttypes.h>

/* Incluir headers das structs para medir */
#include "cluster_manager.h"
#include "node_registry.h"
#include "cluster_io.h"
#include "state.h"

static const char *TAG = "MAIN";

#define ENDAP_BOOT_TASK_STACK_BYTES 16384
#define ENDAP_BOOT_TASK_PRIORITY    2

static void endap_boot_task(void *arg)
{
    (void)arg;

    core_start();
    ESP_LOGI(TAG,
        "STACK boot_task_exit: task=%s min_free=%" PRIu32 " bytes",
        pcTaskGetName(NULL),
        (uint32_t)uxTaskGetStackHighWaterMark(NULL));
    vTaskDelete(NULL);
}

void app_main(void)
{
    ESP_LOGI("SIZE", "cluster_node_t=%u", sizeof(cluster_node_t));
    ESP_LOGI("SIZE", "node_registry_entry_t=%u", sizeof(node_registry_entry_t));
    ESP_LOGI("SIZE", "node_registry_blob_t=%u", sizeof(node_registry_blob_t));
    ESP_LOGI("SIZE", "cluster_io_entry_t=%u", sizeof(cluster_io_entry_t));
    ESP_LOGI("SIZE", "state_entry_t=%u", sizeof(state_entry_t));

    BaseType_t ok = xTaskCreatePinnedToCore(
        endap_boot_task,
        "endap_boot",
        ENDAP_BOOT_TASK_STACK_BYTES,
        NULL,
        ENDAP_BOOT_TASK_PRIORITY,
        NULL,
        0);

    if (ok != pdPASS)
        ESP_LOGE(TAG, "Falha ao criar task de boot do ENDAP");
}

#include "cluster_self_test.h"

#include "cluster_failover.h"
#include "cluster_io.h"
#include "cluster_manager.h"
#include "cluster_metrics.h"
#include "device_profile.h"
#include "io_map.h"
#include "node_registry.h"
#include "state.h"

#include "esp_log.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <inttypes.h>
#include <stdbool.h>

#define TAG "CLUSTER_TEST"

#define CLUSTER_SELF_TEST_ENABLED     0
#define SELF_TEST_TASK_STACK_WORDS 4096
#define SELF_TEST_HEARTBEAT_MS      500
#define SELF_TEST_ONLINE_MS         4000
#define SELF_TEST_OFFLINE_WAIT_MS   7500
#define SELF_TEST_FAILOVER_VIEW_MS  2500
#define SELF_TEST_FAILBACK_VIEW_MS  2000
static TaskHandle_t self_test_task_handle = NULL;
static uint32_t self_node_id = 0;
static volatile bool self_test_running = false;
static volatile bool self_test_requested = false;
static const char *self_test_phase_name = "IDLE";

#if CLUSTER_SELF_TEST_ENABLED
static void cluster_self_test_register_local_io(void)
{
    int input_count = device_profile_input_count();
    int output_count = device_profile_output_count();

    for (int i = 0; i < input_count; i++)
    {
        uint16_t id = device_profile_input_id_at(i);

        if (id != 0U)
            cluster_io_register_local(id);
    }

    for (int i = 0; i < output_count; i++)
    {
        uint16_t id = device_profile_output_id_at(i);

        if (id != 0U)
            cluster_io_register_local(id);
    }
}

static void cluster_self_test_bootstrap_local_cluster(void)
{
    cluster_metrics_t metrics = cluster_get_metrics();

    if (metrics.active || self_node_id == 0)
        return;

    ESP_LOGI(TAG, "Bootstrap local do cluster para bancada");

    cluster_failover_init(self_node_id);
    cluster_self_test_register_local_io();
    cluster_manager_start(self_node_id);
    cluster_io_sync_all();
}

static void cluster_self_test_set_phase(const char *phase)
{
    self_test_phase_name = phase;
    ESP_LOGI(TAG, "Phase: %s", phase);
}

static void cluster_self_test_drive_heartbeats(uint32_t fake_node_id,
                                               uint32_t fake_ip,
                                               uint32_t duration_ms)
{
    uint32_t elapsed = 0;

    while (elapsed < duration_ms)
    {
        cluster_manager_update_node(fake_node_id, fake_ip);
        vTaskDelay(pdMS_TO_TICKS(SELF_TEST_HEARTBEAT_MS));
        elapsed += SELF_TEST_HEARTBEAT_MS;
    }
}

static void cluster_self_test_run_once(void)
{
    cluster_metrics_t metrics;
    uint32_t fake_node_id;
    uint32_t fake_ip = 0x0200A8C0U; /* 192.168.0.2 em little-endian não importa aqui */
    int32_t original_value = 0;
    uint16_t test_output_id = device_profile_preferred_self_test_output_id();

    cluster_self_test_bootstrap_local_cluster();
    metrics = cluster_get_metrics();

    if (!metrics.active)
    {
        ESP_LOGW(TAG, "Cluster ainda nao esta ativo");
        return;
    }

    if (metrics.total_nodes > 1)
    {
        ESP_LOGW(TAG, "Self-test indisponivel com peers reais ativos");
        return;
    }

    if (test_output_id == 0U)
    {
        ESP_LOGW(TAG, "Self-test indisponivel sem saida configurada no perfil");
        return;
    }

    fake_node_id = (self_node_id == UINT32_MAX) ? (self_node_id - 1U) : (self_node_id + 1U);

    state_get_int(test_output_id, &original_value);

    ESP_LOGI(TAG,
        "Iniciando self-test (self=%" PRIu32 ", fake=%" PRIu32 ", output=%u)",
        self_node_id,
        fake_node_id,
        test_output_id);

    cluster_self_test_set_phase("REMOTE_ONLINE");
    state_set_int(test_output_id, 1);
    cluster_io_set_owner(test_output_id, fake_node_id, fake_node_id);

    if (!node_registry_configure(fake_node_id, "gateway", "self-test"))
    {
        ESP_LOGW(TAG, "Falha ao registrar no registry fake gateway %" PRIu32, fake_node_id);
    }

    cluster_self_test_drive_heartbeats(fake_node_id, fake_ip, SELF_TEST_ONLINE_MS);

    cluster_self_test_set_phase("WAIT_OFFLINE");
    vTaskDelay(pdMS_TO_TICKS(SELF_TEST_OFFLINE_WAIT_MS));

    cluster_self_test_set_phase("FAILOVER_VIEW");
    vTaskDelay(pdMS_TO_TICKS(SELF_TEST_FAILOVER_VIEW_MS));

    cluster_self_test_set_phase("REMOTE_BACK");
    cluster_self_test_drive_heartbeats(fake_node_id, fake_ip, SELF_TEST_FAILBACK_VIEW_MS + SELF_TEST_HEARTBEAT_MS);

    cluster_self_test_set_phase("FAILBACK_VIEW");
    vTaskDelay(pdMS_TO_TICKS(SELF_TEST_FAILBACK_VIEW_MS));

    cluster_self_test_set_phase("RESTORE");
    cluster_io_set_owner(test_output_id, self_node_id, self_node_id);
    cluster_io_sync_all();
    cluster_manager_remove_node(fake_node_id);

    if (!node_registry_revoke(fake_node_id))
    {
        ESP_LOGW(TAG, "Falha ao remover no registry fake %" PRIu32, fake_node_id);
    }

    state_set_int(test_output_id, original_value);

    ESP_LOGI(TAG, "Self-test concluido");
}

static void cluster_self_test_task(void *arg)
{
    (void)arg;

    while (1)
    {
        if (!self_test_requested)
        {
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }

        self_test_requested = false;
        self_test_running = true;
        cluster_self_test_run_once();
        cluster_self_test_set_phase("IDLE");
        self_test_running = false;
    }
}
#endif

void cluster_self_test_init(uint32_t node_id)
{
#if !CLUSTER_SELF_TEST_ENABLED
    (void)node_id;
    self_test_phase_name = "DISABLED";
    return;
#else
    if (self_test_task_handle != NULL)
        return;

    self_node_id = node_id;

    xTaskCreatePinnedToCore(
        cluster_self_test_task,
        "cluster_test",
        SELF_TEST_TASK_STACK_WORDS,
        NULL,
        1,
        &self_test_task_handle,
        0
    );

    ESP_LOGI(TAG, "Self-test pronto (node=%" PRIu32 ")", self_node_id);
#endif
}

bool cluster_self_test_available(void)
{
#if CLUSTER_SELF_TEST_ENABLED
    return true;
#else
    return false;
#endif
}

bool cluster_self_test_trigger(void)
{
    if (!cluster_self_test_available())
        return false;

    if (self_test_task_handle == NULL || self_node_id == 0)
        return false;

    if (self_test_running || self_test_requested)
        return false;

    self_test_requested = true;
    return true;
}

bool cluster_self_test_is_running(void)
{
    if (!cluster_self_test_available())
        return false;

    return self_test_running || self_test_requested;
}

const char *cluster_self_test_phase(void)
{
    if (!cluster_self_test_available())
        return "DISABLED";

    return self_test_phase_name;
}

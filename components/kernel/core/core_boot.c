#include "core_supervisor.h"

/* ============================================================
   SYSTEM
============================================================ */

#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_err.h"
#include "driver/uart.h"
#include "driver/gpio.h"
#include "nvs_flash.h"
#include <inttypes.h>
#include <stdbool.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/portmacro.h"

/* ============================================================
   NETWORK
============================================================ */

#include "rs485.h"
#include "rs485_master.h"
#include "rs485_engine.h"
#include "fieldbus.h"
#include "bus_health_monitor.h"

/* ============================================================
   CORE DATA
============================================================ */

#include "state.h"
#include "snapshot.h"

/* ============================================================
   CONTROL ENGINE
============================================================ */

#include "control_kernel.h"
#include "control_loop.h"
#include "io_image.h"

/* ============================================================
   SERVICES
============================================================ */

#include "automation_engine.h"
#include "event_bus.h"
#include "io_command.h"
#include "kernel_metrics.h"
#include "determinism_probe.h"
#include "kernel_trace.h"
#include "input_learning.h"
#include "io_binding.h"
#include "failsafe.h"
#include "phase_load_test.h"

/* ============================================================
   SAFETY
============================================================ */

#include "watchdog.h"
#include "watchdog_ids.h"

/* ============================================================
   CLUSTER
============================================================ */

#include "cluster_discovery.h"
#include "cluster_manager.h"
#include "cluster_failover.h"
#include "cluster_io.h"
#include "cluster_self_test.h"
#include "cluster_transport.h"
#include "node_registry.h"
#include "node_identity.h"
#include "device_profile.h"

/* ============================================================
   IO DRIVER
============================================================ */

#include "io_driver.h"

/* ============================================================
   WIFI / HTTP
============================================================ */

#include "wifi_manager.h"
#include "ethernet_manager.h"
#include "network_ready.h"
#include "http_server.h"
#include "protocol.h"

/* ============================================================
   CONFIG
============================================================ */

#define TAG "CORE_SUP"
#define AUX_TASK_STACK_BYTES           16384
#define AUX_TASK_PERIOD_MS             20
#define CONTROL_LOOP_WD_TIMEOUT_MS     50

static TaskHandle_t aux_task_handle = NULL;
static bool cluster_started = false;
static bool cluster_start_pending = false;
static bool http_started = false;
static bool http_start_pending = false;
static bool aux_stack_logged = false;
static network_ready_link_t pending_cluster_link = NETWORK_READY_LINK_NONE;
static portMUX_TYPE cluster_start_lock = portMUX_INITIALIZER_UNLOCKED;
static portMUX_TYPE http_start_lock = portMUX_INITIALIZER_UNLOCKED;

static uint64_t primary_link_down_since_us = 0;
static bool fallback_activation_attempted = false;

static void core_log_current_task_stack(const char *label)
{
    UBaseType_t free_bytes = uxTaskGetStackHighWaterMark(NULL);
    const char *task_name = pcTaskGetName(NULL);

    ESP_LOGI(TAG,
        "STACK %s: task=%s min_free=%" PRIu32 " bytes",
        label ? label : "snapshot",
        task_name ? task_name : "unknown",
        (uint32_t)free_bytes);
}

static const char *core_transport_name(device_profile_transport_t transport)
{
    switch (transport)
    {
        case DEVICE_PROFILE_TRANSPORT_WIFI:
            return "wifi";
        case DEVICE_PROFILE_TRANSPORT_ETHERNET:
            return "ethernet";
        case DEVICE_PROFILE_TRANSPORT_RS485:
            return "rs485";
        case DEVICE_PROFILE_TRANSPORT_NONE:
        default:
            return "none";
    }
}

static cluster_transport_type_t cluster_transport_type_for_link(network_ready_link_t link)
{
    if (link == NETWORK_READY_LINK_ETHERNET)
        return CLUSTER_TRANSPORT_ETHERNET_UDP;

    if (link == NETWORK_READY_LINK_WIFI_AP || link == NETWORK_READY_LINK_WIFI_STA)
        return CLUSTER_TRANSPORT_WIFI_UDP;

    return CLUSTER_TRANSPORT_NONE;
}

static bool core_snapshot_transport_up(const network_ready_snapshot_t *snapshot,
                                       device_profile_transport_t transport)
{
    if (!snapshot)
        return false;

    switch (transport)
    {
        case DEVICE_PROFILE_TRANSPORT_WIFI:
            return snapshot->wifi_sta_up || snapshot->wifi_ap_up;
        case DEVICE_PROFILE_TRANSPORT_ETHERNET:
            return snapshot->ethernet_up;
        case DEVICE_PROFILE_TRANSPORT_RS485:
            return true;
        case DEVICE_PROFILE_TRANSPORT_NONE:
        default:
            return false;
    }
}

static void register_default_automation(void)
{
    int rule_count = device_profile_default_automation_count();

    if (automation_engine_has_persisted_config())
    {
        ESP_LOGI(TAG, "Automacao restaurada do NVS (%d regra(s))", automation_engine_get_node_count());
        return;
    }

    for (int i = 0; i < rule_count; i++)
    {
        const device_default_automation_t *rule = device_profile_default_automation_at(i);

        if (!rule)
            continue;

        if (automation_engine_add_node(rule->input_id, rule->threshold, rule->output_id))
        {
            ESP_LOGI(TAG,
                "Automacao padrao: input %u -> output %u",
                rule->input_id,
                rule->output_id);
        }
        else
        {
            ESP_LOGW(TAG,
                "Falha ao registrar automacao input %u -> output %u",
                rule->input_id,
                rule->output_id);
        }
    }
}

static void register_local_cluster_io(void)
{
    int input_count = device_profile_input_count();
    int output_count = device_profile_output_count();
    int count = input_count + output_count;

    ESP_LOGI(TAG, "STEP register_local_cluster_io count=%d", count);

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

    ESP_LOGI(TAG, "STEP register_local_cluster_io finished");
}

static void core_start_cluster_transport_explicit(const char *reason,
                                                  cluster_transport_type_t transport_type)
{
    uint32_t node_id;

    if (transport_type == CLUSTER_TRANSPORT_NONE)
    {
        ESP_LOGW(TAG, "%s -> transporte de cluster ainda nao elegivel", reason ? reason : "cluster");
        return;
    }

    if (cluster_started)
    {
        cluster_transport_set_active_type(transport_type);
        ESP_LOGI(TAG,
                 "%s -> cluster transporte atualizado para %s",
                 reason ? reason : "cluster",
                 cluster_transport_active_name());
        return;
    }

    node_identity_init();
    node_id = node_identity_get();

    ESP_LOGI(TAG,
             "%s -> iniciando cluster (node=%" PRIu32 ")",
             reason ? reason : "cluster",
             node_id);

    ESP_LOGI(TAG, "STEP protocol_init");
    protocol_init();
    ESP_LOGI(TAG, "STEP protocol_init ok");

    ESP_LOGI(TAG, "STEP cluster_failover_init");
    cluster_failover_init(node_id);
    ESP_LOGI(TAG, "STEP cluster_failover_init ok");

    ESP_LOGI(TAG, "STEP register_local_cluster_io");
    register_local_cluster_io();
    ESP_LOGI(TAG, "STEP register_local_cluster_io ok");

    ESP_LOGI(TAG, "STEP cluster_manager_start");
    cluster_manager_start(node_id);
    ESP_LOGI(TAG, "STEP cluster_manager_start ok");

    ESP_LOGI(TAG, "STEP cluster_transport_start");
    if (!cluster_transport_start(node_id, transport_type))
    {
        ESP_LOGE(TAG, "Falha ao iniciar cluster transport");
        return;
    }
    ESP_LOGI(TAG, "STEP cluster_transport_start ok");

    ESP_LOGI(TAG, "STEP cluster_discovery_start");
    cluster_discovery_start();
    ESP_LOGI(TAG, "STEP cluster_discovery_start ok");

    ESP_LOGI(TAG, "STEP cluster_io_sync_all skipped");

    ESP_LOGI(TAG, "STEP cluster_self_test_init");
    cluster_self_test_init(node_id);
    ESP_LOGI(TAG, "STEP cluster_self_test_init ok");

    cluster_started = true;

    ESP_LOGI(TAG, "Cluster iniciado com sucesso via %s", cluster_transport_active_name());
    core_log_current_task_stack("cluster_boot");
}

static void core_start_cluster_transport(const char *reason, network_ready_link_t link)
{
    cluster_transport_type_t transport_type = cluster_transport_type_for_link(link);
    core_start_cluster_transport_explicit(reason, transport_type);
}

static void on_network_ready(const network_ready_snapshot_t *snapshot, void *ctx)
{
    (void)ctx;

    if (!snapshot || !snapshot->ready)
        return;

    portENTER_CRITICAL(&cluster_start_lock);

    if (!cluster_start_pending || pending_cluster_link != snapshot->active_link)
    {
        cluster_start_pending = true;
        pending_cluster_link = snapshot->active_link;
    }

    portEXIT_CRITICAL(&cluster_start_lock);

    portENTER_CRITICAL(&http_start_lock);

    if (!http_started && !http_start_pending)
        http_start_pending = true;

    portEXIT_CRITICAL(&http_start_lock);
}

static void cluster_start_poll(void)
{
    network_ready_link_t link = NETWORK_READY_LINK_NONE;
    const char *reason;

    portENTER_CRITICAL(&cluster_start_lock);

    if (!cluster_start_pending)
    {
        portEXIT_CRITICAL(&cluster_start_lock);
        return;
    }

    link = pending_cluster_link;
    cluster_start_pending = false;

    portEXIT_CRITICAL(&cluster_start_lock);

    if (link == NETWORK_READY_LINK_NONE)
        return;

    reason = network_ready_link_name(link);
    core_start_cluster_transport(reason, link);
}

static void http_start_poll(void)
{
    bool should_start = false;

    portENTER_CRITICAL(&http_start_lock);

    if (!http_started && http_start_pending)
    {
        http_start_pending = false;
        should_start = true;
    }

    portEXIT_CRITICAL(&http_start_lock);

    if (!should_start)
        return;

    ESP_LOGI(TAG, "STEP http_server_start");
    http_server_start();
    http_started = true;
    ESP_LOGI(TAG, "STEP http_server_start ok");
}

static void core_network_policy_poll(void)
{
    const device_network_profile_t *network = device_profile_network();
    network_ready_snapshot_t snapshot = {0};
    uint64_t now_us;
    bool primary_up;

    if (!network || network->onboarding_pending)
        return;

    if (network->primary_transport == DEVICE_PROFILE_TRANSPORT_NONE ||
        network->fallback_transport == DEVICE_PROFILE_TRANSPORT_NONE)
    {
        return;
    }

    if (network->fallback_transport == DEVICE_PROFILE_TRANSPORT_RS485)
        return;

    network_ready_get_snapshot(&snapshot);
    primary_up = core_snapshot_transport_up(&snapshot, network->primary_transport);

    if (primary_up)
    {
        primary_link_down_since_us = 0;
        fallback_activation_attempted = false;
        return;
    }

    now_us = (uint64_t)esp_timer_get_time();

    if (primary_link_down_since_us == 0U)
    {
        primary_link_down_since_us = now_us;
        return;
    }

    if (fallback_activation_attempted)
        return;

    if ((now_us - primary_link_down_since_us) < ((uint64_t)network->failover_delay_ms * 1000ULL))
        return;

    fallback_activation_attempted = true;

    if (network->fallback_transport == DEVICE_PROFILE_TRANSPORT_WIFI)
    {
        ESP_LOGW(TAG,
                 "Primary %s indisponivel ha %" PRIu32 " ms -> ativando fallback WiFi",
                 core_transport_name(network->primary_transport),
                 network->failover_delay_ms);
        wifi_manager_init();
    }
    else if (network->fallback_transport == DEVICE_PROFILE_TRANSPORT_ETHERNET)
    {
        ESP_LOGW(TAG,
                 "Primary %s indisponivel ha %" PRIu32 " ms -> ativando fallback Ethernet",
                 core_transport_name(network->primary_transport),
                 network->failover_delay_ms);
        ethernet_manager_init();
    }
}

static void core_try_rs485_cluster_bootstrap(void)
{
    const device_network_profile_t *net = device_profile_network();

    if (!net)
        return;

    if (net->onboarding_pending)
        return;

    if (!net->rs485_supported || !net->rs485_enabled)
        return;

    if (cluster_started || cluster_start_pending)
        return;

    if (net->primary_transport != DEVICE_PROFILE_TRANSPORT_RS485)
        return;

    ESP_LOGI(TAG, "RS485 bootstrap: iniciando cluster sem dependencia de IP");
    core_start_cluster_transport_explicit("rs485-bootstrap", CLUSTER_TRANSPORT_RS485);
}

/* ============================================================
   INFRA INIT
============================================================ */

static void core_init_infrastructure(void)
{
    ESP_LOGI(TAG, "Init Infrastructure");

    if (device_profile_should_start_rs485_on_boot())
    {
        /*
         * TTL485-V2.0: modulo TTL<->RS485 sem pino DE/RE exposto ao firmware.
         * Operar com de_pin = -1 para deixar a direcao sob responsabilidade
         * do proprio modulo auto-direction.
         */
        rs485_init(&(rs485_config_t)
        {
            .uart_num           = UART_NUM_2,
            .tx_pin             = 25,
            .rx_pin             = 26,
            .de_pin             = -1,
            .baudrate           = 9600,
            .auto_direction     = true,
            .tx_guard_us        = 3000,
            .rx_recovery_us     = 2000,
            .tx_done_timeout_ms = 20,
        });
    }
    else
    {
        ESP_LOGI(TAG, "RS485 desativado no perfil -> GPIO25/GPIO26 livres para IO");
    }
}

/* ============================================================
   DOMAIN INIT
============================================================ */

static void core_init_domain(void)
{
    ESP_LOGI(TAG, "Init Domain");

    state_init();
    io_image_init();
    fieldbus_init();
    control_kernel_init();
}

static void core_aux_task(void *arg)
{
    (void)arg;

    while (1)
    {
        fieldbus_process_logs();
        http_server_process();
        http_start_poll();
        cluster_start_poll();
        core_network_policy_poll();
        cluster_manager_process();
        node_registry_process();
        io_driver_process();
        snapshot_process();
        determinism_probe_process();
        kernel_trace_process();
        watchdog_check();

        if (cluster_started && !aux_stack_logged)
        {
            aux_stack_logged = true;
            core_log_current_task_stack("aux_post_cluster");
        }

        vTaskDelay(pdMS_TO_TICKS(AUX_TASK_PERIOD_MS));
    }
}

/* ============================================================
   SERVICES INIT
============================================================ */

static void core_init_services(void)
{
    const device_network_profile_t *net = NULL;
    esp_err_t gpio_isr_err;

    phase_load_test_clear();
    ESP_LOGI(TAG, "Phase load test limpo no boot");

    ESP_LOGI(TAG, "Init Services");

    event_bus_init();
    kernel_metrics_init();
    bus_health_init();

    watchdog_init();

    snapshot_init();
    automation_engine_init();
    input_learning_init();
    io_binding_init();
    failsafe_init();
    register_default_automation();
    io_command_init();
    determinism_probe_init();
    kernel_trace_init();

    io_driver_init();
    network_ready_init();
    network_ready_register_callback(on_network_ready, NULL);
    node_registry_init();

    gpio_isr_err = gpio_install_isr_service(ESP_INTR_FLAG_IRAM);
    if (gpio_isr_err != ESP_OK && gpio_isr_err != ESP_ERR_INVALID_STATE)
        ESP_LOGE(TAG, "Falha ao instalar GPIO ISR service: %s", esp_err_to_name(gpio_isr_err));

    net = device_profile_network();
    ESP_LOGI(TAG,
             "NET policy: onboarding=%d primary=%s fallback=%s wifi_enabled=%d eth_enabled=%d rs485_enabled=%d failover=%" PRIu32 "ms",
             net ? (net->onboarding_pending ? 1 : 0) : 0,
             net ? core_transport_name(net->primary_transport) : "none",
             net ? core_transport_name(net->fallback_transport) : "none",
             net ? (net->wifi_enabled ? 1 : 0) : 0,
             net ? (net->ethernet_enabled ? 1 : 0) : 0,
             net ? (net->rs485_enabled ? 1 : 0) : 0,
             net ? net->failover_delay_ms : 0U);

    if (device_profile_should_start_wifi_on_boot())
        wifi_manager_init();

    if (device_profile_should_start_ethernet_on_boot())
        ethernet_manager_init();

    core_try_rs485_cluster_bootstrap();
}

/* ============================================================
   CORE START
============================================================ */

void core_start(void)
{
    esp_err_t err = nvs_flash_init();

    if (err == ESP_ERR_NVS_NO_FREE_PAGES ||
        err == ESP_ERR_NVS_NEW_VERSION_FOUND)
    {
        nvs_flash_erase();
        nvs_flash_init();
    }

    ESP_LOGI(TAG, "Boot ENDAP");

    core_init_infrastructure();
    core_init_domain();
    core_init_services();

    ESP_LOGI(TAG, "Starting Control Loop");

    control_loop_start();
    watchdog_register(WD_CONTROL_LOOP, CONTROL_LOOP_WD_TIMEOUT_MS);
    core_log_current_task_stack("boot_post_control_loop");

    if (aux_task_handle == NULL)
    {
        xTaskCreatePinnedToCore(
            core_aux_task,
            "core_aux",
            AUX_TASK_STACK_BYTES,
            NULL,
            1,
            &aux_task_handle,
            0
        );
    }
}

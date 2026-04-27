#include "wifi_manager.h"

#include "captive_dns.h"
#include "network_ready.h"
#include "node_identity.h"

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "nvs_flash.h"

#include "freertos/FreeRTOS.h"
#include "freertos/portmacro.h"
#include "freertos/task.h"

#include "lwip/ip4_addr.h"

#include <inttypes.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TAG "WIFI"

#define WIFI_NAMESPACE "wifi"
#define DEFAULT_AP_SSID_PREFIX "ENDAP_SETUP-"
#define DEFAULT_AP_SSID_FALLBACK "ENDAP_SETUP"
#define DEFAULT_AP_PASS "12345678"
#define WIFI_STA_MAX_RETRIES 5U
#define WIFI_SSID_MAX_LEN 32U
#define WIFI_PASS_MAX_LEN 64U
#define WIFI_CLUSTER_SCAN_RETRY_MS 3000U
#define WIFI_CLUSTER_SCAN_HOST_BACKOFF_MS 15000U
#define WIFI_CLUSTER_SCAN_START_DELAY_MS 1500U
#define WIFI_CLUSTER_SCAN_MAX_APS 16U
#define WIFI_CLUSTER_AUTO_JOIN_FROM_FALLBACK_AP 0U

static bool wifi_initialized = false;
static bool wifi_driver_started = false;
static esp_netif_t *wifi_ap_netif = NULL;
static TaskHandle_t wifi_cluster_scan_task_handle = NULL;
static portMUX_TYPE wifi_status_lock = portMUX_INITIALIZER_UNLOCKED;
static portMUX_TYPE wifi_cluster_lock = portMUX_INITIALIZER_UNLOCKED;
static wifi_manager_status_t wifi_status = {
    .retry_limit = WIFI_STA_MAX_RETRIES,
    .state = WIFI_MANAGER_STATE_IDLE,
};
static uint32_t wifi_cluster_self_node_id = 0U;
static uint8_t wifi_ap_client_count = 0U;

typedef enum
{
    WIFI_TARGET_MODE_NONE = 0,
    WIFI_TARGET_MODE_STA = 1,
    WIFI_TARGET_MODE_AP_FALLBACK = 2,
} wifi_target_mode_t;

static wifi_target_mode_t wifi_target_mode = WIFI_TARGET_MODE_NONE;

static esp_err_t wifi_build_sta_config(const char *ssid,
                                       const char *pass,
                                       wifi_config_t *out_sta_config);
static esp_err_t wifi_manager_start_sta_with_config(wifi_config_t *sta_config,
                                                    const char *ssid,
                                                    bool credentials_saved);

static uint32_t wifi_manager_default_ap_ip_addr(void)
{
    ip4_addr_t ap_ip;

    IP4_ADDR(&ap_ip, 192, 168, 4, 1);
    return ap_ip.addr;
}

static uint32_t wifi_manager_default_ap_netmask_addr(void)
{
    ip4_addr_t ap_netmask;

    IP4_ADDR(&ap_netmask, 255, 255, 255, 0);
    return ap_netmask.addr;
}

static void wifi_manager_build_cluster_ap_ssid(char *buf, size_t buf_size)
{
    if (!buf || buf_size == 0U)
        return;

    if (wifi_cluster_self_node_id == 0U)
    {
        snprintf(buf, buf_size, "%s", DEFAULT_AP_SSID_FALLBACK);
        return;
    }

    snprintf(buf,
             buf_size,
             "%s%08" PRIX32,
             DEFAULT_AP_SSID_PREFIX,
             wifi_cluster_self_node_id);
}

static bool wifi_manager_parse_cluster_peer_id(const char *ssid, uint32_t *out_node_id)
{
    const char *suffix;
    char *endptr = NULL;
    unsigned long parsed;
    size_t prefix_len;

    if (!ssid)
        return false;

    prefix_len = strlen(DEFAULT_AP_SSID_PREFIX);
    if (strncmp(ssid, DEFAULT_AP_SSID_PREFIX, prefix_len) != 0)
        return false;

    suffix = ssid + prefix_len;
    if (strlen(suffix) != 8U)
        return false;

    parsed = strtoul(suffix, &endptr, 16);
    if (!endptr || *endptr != '\0')
        return false;

    if (out_node_id)
        *out_node_id = (uint32_t)parsed;

    return true;
}

static bool wifi_manager_find_cluster_host(char *out_ssid,
                                           size_t out_ssid_size,
                                           uint32_t *out_node_id,
                                           int8_t *out_rssi)
{
    wifi_scan_config_t scan_cfg = {0};
    wifi_ap_record_t records[WIFI_CLUSTER_SCAN_MAX_APS];
    uint16_t count = WIFI_CLUSTER_SCAN_MAX_APS;
    uint32_t best_node_id = 0U;
    int8_t best_rssi = 0;
    bool found = false;
    esp_err_t err;

    if (wifi_cluster_self_node_id == 0U)
        return false;

    memset(records, 0, sizeof(records));

    err = esp_wifi_scan_start(&scan_cfg, true);
    if (err != ESP_OK)
    {
        ESP_LOGW(TAG, "Scan de peers WiFi falhou: %s", esp_err_to_name(err));
        return false;
    }

    err = esp_wifi_scan_get_ap_records(&count, records);
    if (err != ESP_OK)
    {
        ESP_LOGW(TAG, "Falha ao ler resultados do scan WiFi: %s", esp_err_to_name(err));
        return false;
    }

    for (uint16_t i = 0; i < count; i++)
    {
        uint32_t candidate_node_id = 0U;
        const char *candidate_ssid = (const char *)records[i].ssid;

        if (!wifi_manager_parse_cluster_peer_id(candidate_ssid, &candidate_node_id))
            continue;

        if (candidate_node_id == 0U || candidate_node_id >= wifi_cluster_self_node_id)
            continue;

        if (!found ||
            candidate_node_id < best_node_id ||
            (candidate_node_id == best_node_id && records[i].rssi > best_rssi))
        {
            best_node_id = candidate_node_id;
            best_rssi = records[i].rssi;
            found = true;

            if (out_ssid && out_ssid_size > 0U)
            {
                strncpy(out_ssid, candidate_ssid, out_ssid_size - 1U);
                out_ssid[out_ssid_size - 1U] = '\0';
            }
        }
    }

    if (found)
    {
        if (out_node_id)
            *out_node_id = best_node_id;

        if (out_rssi)
            *out_rssi = best_rssi;
    }

    return found;
}

static esp_err_t wifi_manager_join_cluster_peer(const char *peer_ssid)
{
    wifi_config_t sta_config = {0};
    esp_err_t err;

    if (!peer_ssid || peer_ssid[0] == '\0')
        return ESP_ERR_INVALID_ARG;

    err = wifi_build_sta_config(peer_ssid, DEFAULT_AP_PASS, &sta_config);
    if (err != ESP_OK)
        return err;

    ESP_LOGI(TAG, "Peer host encontrado (%s). Entrando em STA do cluster", peer_ssid);
    return wifi_manager_start_sta_with_config(&sta_config, peer_ssid, false);
}

static void wifi_manager_cluster_scan_task(void *arg)
{
    (void)arg;

#if !WIFI_CLUSTER_AUTO_JOIN_FROM_FALLBACK_AP
    while (1)
    {
        vTaskDelay(pdMS_TO_TICKS(10000));
    }
#else
    vTaskDelay(pdMS_TO_TICKS(WIFI_CLUSTER_SCAN_START_DELAY_MS));

    while (1)
    {
        wifi_manager_status_t status = {0};
        uint32_t delay_ms = WIFI_CLUSTER_SCAN_RETRY_MS;
        uint8_t ap_clients = 0U;

        wifi_manager_get_status(&status);
        portENTER_CRITICAL(&wifi_cluster_lock);
        ap_clients = wifi_ap_client_count;
        portEXIT_CRITICAL(&wifi_cluster_lock);

        if (status.ap_fallback_active &&
            !status.credentials_saved &&
            ap_clients == 0U &&
            wifi_target_mode == WIFI_TARGET_MODE_AP_FALLBACK)
        {
            char peer_ssid[WIFI_SSID_MAX_LEN + 1U] = {0};
            uint32_t peer_node_id = 0U;
            int8_t peer_rssi = 0;

            if (wifi_manager_find_cluster_host(peer_ssid,
                                               sizeof(peer_ssid),
                                               &peer_node_id,
                                               &peer_rssi))
            {
                ESP_LOGI(TAG,
                         "Peer WiFi eleito: node=%" PRIu32 " ssid=%s rssi=%d",
                         peer_node_id,
                         peer_ssid,
                         (int)peer_rssi);

                if (wifi_manager_join_cluster_peer(peer_ssid) == ESP_OK)
                {
                    vTaskDelay(pdMS_TO_TICKS(WIFI_CLUSTER_SCAN_RETRY_MS));
                    continue;
                }
            }
            else
            {
                delay_ms = WIFI_CLUSTER_SCAN_HOST_BACKOFF_MS;
            }
        }

        vTaskDelay(pdMS_TO_TICKS(delay_ms));
    }
#endif
}

static void wifi_manager_start_cluster_scan_task(void)
{
#if !WIFI_CLUSTER_AUTO_JOIN_FROM_FALLBACK_AP
    ESP_LOGW(TAG, "Auto-join do cluster WiFi desabilitado; AP local de gestao sera mantido");
    return;
#else
    BaseType_t ok;

    if (wifi_cluster_scan_task_handle)
        return;

    ok = xTaskCreatePinnedToCore(
        wifi_manager_cluster_scan_task,
        "wifi_cluster",
        4096,
        NULL,
        3,
        &wifi_cluster_scan_task_handle,
        0);

    if (ok != pdPASS)
    {
        wifi_cluster_scan_task_handle = NULL;
        ESP_LOGW(TAG, "Falha ao iniciar task de scan do cluster WiFi");
    }
#endif
}

static void wifi_status_copy_ssid_locked(const char *ssid)
{
    size_t len = 0U;

    memset(wifi_status.ssid, 0, sizeof(wifi_status.ssid));

    if (!ssid)
        return;

    len = strnlen(ssid, WIFI_SSID_MAX_LEN);
    memcpy(wifi_status.ssid, ssid, len);
    wifi_status.ssid[len] = '\0';
}

static void wifi_status_mark_initialized(void)
{
    portENTER_CRITICAL(&wifi_status_lock);
    wifi_status.initialized = true;
    wifi_status.retry_limit = WIFI_STA_MAX_RETRIES;
    portEXIT_CRITICAL(&wifi_status_lock);
}

static void wifi_status_mark_sta_connecting(const char *ssid, bool credentials_saved)
{
    portENTER_CRITICAL(&wifi_status_lock);
    wifi_status.initialized = true;
    wifi_status.credentials_saved = credentials_saved;
    wifi_status.sta_requested = true;
    wifi_status.sta_connected = false;
    wifi_status.ap_fallback_active = false;
    wifi_status.retry_count = 0U;
    wifi_status.last_disconnect_reason = 0U;
    wifi_status.state = WIFI_MANAGER_STATE_STA_CONNECTING;
    wifi_status.retry_limit = WIFI_STA_MAX_RETRIES;
    wifi_status_copy_ssid_locked(ssid);
    portEXIT_CRITICAL(&wifi_status_lock);
}

static void wifi_status_mark_sta_retry(uint8_t retry_count, uint8_t reason)
{
    portENTER_CRITICAL(&wifi_status_lock);
    wifi_status.initialized = true;
    wifi_status.sta_requested = true;
    wifi_status.sta_connected = false;
    wifi_status.ap_fallback_active = false;
    wifi_status.retry_count = retry_count;
    wifi_status.last_disconnect_reason = reason;
    wifi_status.state = WIFI_MANAGER_STATE_STA_RETRYING;
    wifi_status.retry_limit = WIFI_STA_MAX_RETRIES;
    portEXIT_CRITICAL(&wifi_status_lock);
}

static void wifi_status_mark_sta_connected(void)
{
    portENTER_CRITICAL(&wifi_status_lock);
    wifi_status.initialized = true;
    wifi_status.sta_requested = true;
    wifi_status.sta_connected = true;
    wifi_status.ap_fallback_active = false;
    wifi_status.retry_count = 0U;
    wifi_status.last_disconnect_reason = 0U;
    wifi_status.state = WIFI_MANAGER_STATE_STA_CONNECTED;
    wifi_status.retry_limit = WIFI_STA_MAX_RETRIES;
    portEXIT_CRITICAL(&wifi_status_lock);
}

static void wifi_status_mark_ap_fallback(bool credentials_saved, const char *ssid, uint8_t reason)
{
    portENTER_CRITICAL(&wifi_status_lock);
    wifi_status.initialized = true;
    wifi_status.credentials_saved = credentials_saved;
    wifi_status.sta_requested = false;
    wifi_status.sta_connected = false;
    wifi_status.ap_fallback_active = true;
    wifi_status.retry_count = 0U;
    wifi_status.last_disconnect_reason = reason;
    wifi_status.state = WIFI_MANAGER_STATE_AP_FALLBACK;
    wifi_status.retry_limit = WIFI_STA_MAX_RETRIES;
    wifi_status_copy_ssid_locked(ssid);
    portEXIT_CRITICAL(&wifi_status_lock);
}

static void wifi_status_mark_error(const char *ssid, bool credentials_saved)
{
    portENTER_CRITICAL(&wifi_status_lock);
    wifi_status.initialized = true;
    wifi_status.credentials_saved = credentials_saved;
    wifi_status.sta_requested = false;
    wifi_status.sta_connected = false;
    wifi_status.ap_fallback_active = false;
    wifi_status.state = WIFI_MANAGER_STATE_ERROR;
    wifi_status.retry_limit = WIFI_STA_MAX_RETRIES;
    wifi_status_copy_ssid_locked(ssid);
    portEXIT_CRITICAL(&wifi_status_lock);
}

static esp_err_t wifi_ignore_disconnect_err(esp_err_t err)
{
    if (err == ESP_OK ||
        err == ESP_ERR_WIFI_NOT_INIT ||
        err == ESP_ERR_WIFI_NOT_STARTED ||
        err == ESP_ERR_WIFI_NOT_CONNECT)
    {
        return ESP_OK;
    }

    return err;
}

static esp_err_t wifi_ignore_stop_err(esp_err_t err)
{
    if (err == ESP_OK ||
        err == ESP_ERR_WIFI_NOT_INIT ||
        err == ESP_ERR_WIFI_NOT_STARTED)
    {
        return ESP_OK;
    }

    return err;
}

static esp_err_t wifi_build_sta_config(const char *ssid,
                                       const char *pass,
                                       wifi_config_t *out_sta_config)
{
    size_t ssid_len;
    size_t pass_len;

    if (!ssid || !out_sta_config)
        return ESP_ERR_INVALID_ARG;

    ssid_len = strnlen(ssid, WIFI_SSID_MAX_LEN + 1U);
    pass_len = strnlen(pass ? pass : "", WIFI_PASS_MAX_LEN + 1U);

    if (ssid_len == 0U || ssid_len > WIFI_SSID_MAX_LEN || pass_len > WIFI_PASS_MAX_LEN)
        return ESP_ERR_INVALID_ARG;

    memset(out_sta_config, 0, sizeof(*out_sta_config));
    memcpy(out_sta_config->sta.ssid, ssid, ssid_len);

    if (pass && pass_len > 0U)
        memcpy(out_sta_config->sta.password, pass, pass_len);

    out_sta_config->sta.scan_method = WIFI_ALL_CHANNEL_SCAN;
    out_sta_config->sta.sort_method = WIFI_CONNECT_AP_BY_SIGNAL;
    out_sta_config->sta.threshold.authmode = WIFI_AUTH_OPEN;
    out_sta_config->sta.pmf_cfg.capable = true;
    out_sta_config->sta.pmf_cfg.required = false;

    return ESP_OK;
}

/* ============================================================
   STORAGE (NVS)
============================================================ */

static bool load_wifi_credentials(char *ssid,
                                  size_t ssid_size,
                                  char *pass,
                                  size_t pass_size)
{
    nvs_handle_t nvs;

    if (!ssid || !pass || ssid_size == 0U || pass_size == 0U)
        return false;

    if (nvs_open(WIFI_NAMESPACE, NVS_READONLY, &nvs) != ESP_OK)
        return false;

    size_t ssid_len = ssid_size;
    size_t pass_len = pass_size;

    esp_err_t err1 = nvs_get_str(nvs, "ssid", ssid, &ssid_len);
    esp_err_t err2 = nvs_get_str(nvs, "pass", pass, &pass_len);

    nvs_close(nvs);

    if (err1 != ESP_OK || err2 != ESP_OK)
        return false;

    if (strlen(ssid) == 0)
        return false;

    return true;
}

static esp_err_t save_wifi_credentials(const char *ssid, const char *pass)
{
    nvs_handle_t nvs;
    esp_err_t err;

    err = nvs_open(WIFI_NAMESPACE, NVS_READWRITE, &nvs);
    if (err != ESP_OK)
        return err;

    err = nvs_set_str(nvs, "ssid", ssid);
    if (err == ESP_OK)
        err = nvs_set_str(nvs, "pass", pass ? pass : "");
    if (err == ESP_OK)
        err = nvs_commit(nvs);

    nvs_close(nvs);
    return err;
}

static esp_err_t wifi_manager_start_ap_fallback(bool credentials_saved,
                                                const char *ssid,
                                                uint8_t reason)
{
    wifi_config_t ap_config = {0};
    char ap_ssid[WIFI_SSID_MAX_LEN + 1U] = {0};
    esp_err_t err;
    const char *fallback_reason = credentials_saved
        ? "Iniciando modo AP (fallback apos falha STA)"
        : "Iniciando modo AP (sem credenciais)";

    ESP_LOGW(TAG, "%s", fallback_reason);
    wifi_manager_build_cluster_ap_ssid(ap_ssid, sizeof(ap_ssid));

    wifi_target_mode = WIFI_TARGET_MODE_AP_FALLBACK;
    wifi_status_mark_ap_fallback(credentials_saved, ap_ssid, reason);
    network_ready_publish_down(NETWORK_READY_LINK_WIFI_STA);

    if (wifi_ignore_disconnect_err(esp_wifi_disconnect()) != ESP_OK)
        ESP_LOGW(TAG, "Falha ao desconectar STA antes do fallback AP");

    if (wifi_driver_started && wifi_ignore_stop_err(esp_wifi_stop()) != ESP_OK)
        ESP_LOGW(TAG, "Falha ao parar WiFi antes do fallback AP");

    if (wifi_ap_netif)
    {
        esp_netif_ip_info_t ip_info = {0};

        IP4_ADDR(&ip_info.ip, 192, 168, 4, 1);
        IP4_ADDR(&ip_info.gw, 192, 168, 4, 1);
        IP4_ADDR(&ip_info.netmask, 255, 255, 255, 0);

        esp_netif_dhcps_stop(wifi_ap_netif);
        esp_netif_set_ip_info(wifi_ap_netif, &ip_info);
        esp_netif_dhcps_start(wifi_ap_netif);
    }

    memcpy(ap_config.ap.ssid, ap_ssid, strlen(ap_ssid));
    memcpy(ap_config.ap.password, DEFAULT_AP_PASS, sizeof(DEFAULT_AP_PASS) - 1U);
    ap_config.ap.ssid_len = (uint8_t)strlen(ap_ssid);
    ap_config.ap.max_connection = 4;
    ap_config.ap.authmode = WIFI_AUTH_WPA_WPA2_PSK;

    err = esp_wifi_set_mode(WIFI_MODE_APSTA);
    if (err != ESP_OK)
        goto fail;

    err = esp_wifi_set_config(WIFI_IF_AP, &ap_config);
    if (err != ESP_OK)
        goto fail;

    err = esp_wifi_start();
    if (err != ESP_OK)
        goto fail;

    wifi_driver_started = true;
    captive_dns_start();
    ESP_LOGI(TAG, "AP de cluster disponivel: ssid=%s senha=%s", ap_ssid, DEFAULT_AP_PASS);
    return ESP_OK;

fail:
    wifi_status_mark_error(ap_ssid[0] ? ap_ssid : ssid, credentials_saved);
    ESP_LOGE(TAG, "Falha ao iniciar fallback AP: %s", esp_err_to_name(err));
    return err;
}

static esp_err_t wifi_manager_start_sta_with_config(wifi_config_t *sta_config,
                                                    const char *ssid,
                                                    bool credentials_saved)
{
    esp_err_t err;

    if (!sta_config || !ssid)
        return ESP_ERR_INVALID_ARG;

    captive_dns_stop();
    wifi_target_mode = WIFI_TARGET_MODE_STA;
    wifi_status_mark_sta_connecting(ssid, credentials_saved);
    network_ready_publish_down(NETWORK_READY_LINK_WIFI_AP);
    network_ready_publish_down(NETWORK_READY_LINK_WIFI_STA);

    if (wifi_ignore_disconnect_err(esp_wifi_disconnect()) != ESP_OK)
        ESP_LOGW(TAG, "Falha ao desconectar WiFi antes do modo STA");

    if (wifi_driver_started && wifi_ignore_stop_err(esp_wifi_stop()) != ESP_OK)
        ESP_LOGW(TAG, "Falha ao parar WiFi antes do modo STA");

    err = esp_wifi_set_mode(WIFI_MODE_STA);
    if (err != ESP_OK)
        goto fail;

    err = esp_wifi_set_config(WIFI_IF_STA, sta_config);
    if (err != ESP_OK)
        goto fail;

    err = esp_wifi_start();
    if (err != ESP_OK)
        goto fail;

    wifi_driver_started = true;
    return ESP_OK;

fail:
    wifi_status_mark_error(ssid, credentials_saved);
    ESP_LOGE(TAG, "Falha ao iniciar modo STA: %s", esp_err_to_name(err));
    return wifi_manager_start_ap_fallback(credentials_saved, ssid, 0U);
}

/* ============================================================
   PUBLIC API
============================================================ */

esp_err_t wifi_manager_save(const char *ssid, const char *pass)
{
    wifi_config_t sta_config = {0};
    esp_err_t err;

    err = wifi_build_sta_config(ssid, pass, &sta_config);
    if (err != ESP_OK)
        return err;

    ESP_LOGI(TAG, "Salvando novas credenciais para SSID '%s'...", ssid);

    err = save_wifi_credentials(ssid, pass ? pass : "");
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "Falha ao salvar credenciais: %s", esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(TAG, "Reconfigurando WiFi para modo STA");
    return wifi_manager_start_sta_with_config(&sta_config, ssid, true);
}

esp_err_t wifi_manager_try_reconnect(void)
{
    char ssid[WIFI_SSID_MAX_LEN + 1U] = {0};
    char pass[WIFI_PASS_MAX_LEN + 1U] = {0};
    wifi_config_t sta_config = {0};

    if (!wifi_initialized)
        wifi_manager_init();

    if (load_wifi_credentials(ssid, sizeof(ssid), pass, sizeof(pass)))
    {
        esp_err_t err = wifi_build_sta_config(ssid, pass, &sta_config);
        if (err != ESP_OK)
            return err;

        ESP_LOGI(TAG, "Recovery: tentando reconectar STA em '%s'", ssid);
        return wifi_manager_start_sta_with_config(&sta_config, ssid, true);
    }

    ESP_LOGW(TAG, "Recovery: sem credenciais, mantendo AP de fallback");
    return wifi_manager_start_ap_fallback(false, "", 0U);
}


esp_err_t wifi_manager_scan_networks(wifi_manager_scan_result_t *out_results,
                                     size_t max_results,
                                     size_t *out_count)
{
    wifi_scan_config_t scan_cfg = {0};
    wifi_ap_record_t records[WIFI_CLUSTER_SCAN_MAX_APS];
    uint16_t count = WIFI_CLUSTER_SCAN_MAX_APS;
    size_t written = 0U;
    esp_err_t err;

    if (!out_results || max_results == 0U || !out_count)
        return ESP_ERR_INVALID_ARG;

    *out_count = 0U;

    if (!wifi_initialized || !wifi_driver_started)
        return ESP_ERR_INVALID_STATE;

    if (max_results < count)
        count = (uint16_t)max_results;

    memset(records, 0, sizeof(records));
    err = esp_wifi_scan_start(&scan_cfg, true);
    if (err != ESP_OK)
        return err;

    err = esp_wifi_scan_get_ap_records(&count, records);
    if (err != ESP_OK)
        return err;

    for (uint16_t i = 0; i < count && written < max_results; i++)
    {
        bool duplicate = false;

        if (records[i].ssid[0] == '\0')
            continue;

        for (size_t j = 0; j < written; j++)
        {
            if (strncmp(out_results[j].ssid, (const char *)records[i].ssid, sizeof(out_results[j].ssid)) == 0)
            {
                duplicate = true;
                break;
            }
        }

        if (duplicate)
            continue;

        memset(&out_results[written], 0, sizeof(out_results[written]));
        strncpy(out_results[written].ssid, (const char *)records[i].ssid, sizeof(out_results[written].ssid) - 1U);
        out_results[written].ssid[sizeof(out_results[written].ssid) - 1U] = '\0';
        out_results[written].rssi = records[i].rssi;
        out_results[written].authmode = (uint8_t)records[i].authmode;
        written++;
    }

    *out_count = written;
    return ESP_OK;
}

esp_err_t wifi_manager_force_recovery_ap(void)
{
    wifi_manager_status_t status = {0};

    if (!wifi_initialized)
        wifi_manager_init();

    wifi_manager_get_status(&status);
    ESP_LOGW(TAG, "Recovery: forçando AP de fallback");
    return wifi_manager_start_ap_fallback(status.credentials_saved, status.ssid, 0U);
}

void wifi_manager_get_status(wifi_manager_status_t *out_status)
{
    if (!out_status)
        return;

    portENTER_CRITICAL(&wifi_status_lock);
    *out_status = wifi_status;
    portEXIT_CRITICAL(&wifi_status_lock);
}

bool wifi_manager_has_credentials(void)
{
    bool has_credentials = false;

    portENTER_CRITICAL(&wifi_status_lock);
    has_credentials = wifi_status.credentials_saved;
    portEXIT_CRITICAL(&wifi_status_lock);

    return has_credentials;
}

const char *wifi_manager_state_name(wifi_manager_state_t state)
{
    switch (state)
    {
        case WIFI_MANAGER_STATE_STA_CONNECTING:
            return "sta-connecting";
        case WIFI_MANAGER_STATE_STA_RETRYING:
            return "sta-retrying";
        case WIFI_MANAGER_STATE_STA_CONNECTED:
            return "sta-connected";
        case WIFI_MANAGER_STATE_AP_FALLBACK:
            return "ap-fallback";
        case WIFI_MANAGER_STATE_ERROR:
            return "error";
        case WIFI_MANAGER_STATE_IDLE:
        default:
            return "idle";
    }
}

/* ============================================================
   EVENT HANDLER
============================================================ */

static void wifi_event_handler(void* arg,
                              esp_event_base_t event_base,
                              int32_t event_id,
                              void* event_data)
{
    (void)arg;

    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START)
    {
        if (wifi_target_mode == WIFI_TARGET_MODE_STA)
        {
            ESP_LOGI(TAG, "Conectando WiFi em STA...");
            esp_wifi_connect();
        }
    }
    else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_CONNECTED)
    {
        if (wifi_target_mode == WIFI_TARGET_MODE_STA)
            ESP_LOGI(TAG, "STA associada ao ponto de acesso");
    }

    else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED)
    {
        const wifi_event_sta_disconnected_t *event = (const wifi_event_sta_disconnected_t *)event_data;
        uint8_t reason = event ? (uint8_t)event->reason : 0U;
        uint8_t retry_count = 0U;

        ESP_LOGW(TAG, "WiFi STA desconectado (reason=%" PRIu8 ")", reason);
        network_ready_publish_down(NETWORK_READY_LINK_WIFI_STA);

        if (wifi_target_mode != WIFI_TARGET_MODE_STA)
            return;

        portENTER_CRITICAL(&wifi_status_lock);
        retry_count = wifi_status.retry_count;
        portEXIT_CRITICAL(&wifi_status_lock);

        if (retry_count < WIFI_STA_MAX_RETRIES)
        {
            retry_count++;
            wifi_status_mark_sta_retry(retry_count, reason);
            ESP_LOGW(TAG,
                     "Tentando reconectar ao WiFi (%" PRIu8 "/%u)",
                     retry_count,
                     WIFI_STA_MAX_RETRIES);
            esp_wifi_connect();
        }
        else
        {
            wifi_manager_status_t status = {0};

            wifi_manager_get_status(&status);
            ESP_LOGE(TAG, "Falha de STA apos %u tentativas → fallback AP", WIFI_STA_MAX_RETRIES);
            wifi_manager_start_ap_fallback(status.credentials_saved, status.ssid, reason);
        }
    }
    else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_AP_START)
    {
        network_ready_publish_up(
            NETWORK_READY_LINK_WIFI_AP,
            wifi_manager_default_ap_ip_addr(),
            wifi_manager_default_ap_netmask_addr());
    }
    else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_AP_STACONNECTED)
    {
        portENTER_CRITICAL(&wifi_cluster_lock);
        wifi_ap_client_count++;
        portEXIT_CRITICAL(&wifi_cluster_lock);
        ESP_LOGI(TAG, "Cliente conectado ao AP do cluster");
    }
    else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_AP_STADISCONNECTED)
    {
        portENTER_CRITICAL(&wifi_cluster_lock);
        if (wifi_ap_client_count > 0U)
            wifi_ap_client_count--;
        portEXIT_CRITICAL(&wifi_cluster_lock);
        ESP_LOGI(TAG, "Cliente desconectado do AP do cluster");
    }
    else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_AP_STOP)
    {
        portENTER_CRITICAL(&wifi_cluster_lock);
        wifi_ap_client_count = 0U;
        portEXIT_CRITICAL(&wifi_cluster_lock);
        network_ready_publish_down(NETWORK_READY_LINK_WIFI_AP);
    }

    else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP)
    {
        const ip_event_got_ip_t *event = (const ip_event_got_ip_t *)event_data;

        ESP_LOGI(TAG, "WiFi conectado com IP");
        wifi_status_mark_sta_connected();
        network_ready_publish_up(
            NETWORK_READY_LINK_WIFI_STA,
            event ? event->ip_info.ip.addr : 0U,
            event ? event->ip_info.netmask.addr : 0U);
    }
    else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_LOST_IP)
    {
        ESP_LOGW(TAG, "WiFi STA perdeu IP");
        network_ready_publish_down(NETWORK_READY_LINK_WIFI_STA);
    }
}

/* ============================================================
   INIT
============================================================ */

void wifi_manager_init(void)
{
    if (wifi_initialized)
    {
        ESP_LOGW(TAG, "WiFi já inicializado");
        return;
    }

    wifi_initialized = true;
    wifi_status_mark_initialized();

    ESP_LOGI(TAG, "Inicializando WiFi Manager");
    node_identity_init();
    wifi_cluster_self_node_id = node_identity_get();
    portENTER_CRITICAL(&wifi_cluster_lock);
    wifi_ap_client_count = 0U;
    portEXIT_CRITICAL(&wifi_cluster_lock);

    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND)
    {
        nvs_flash_erase();
        nvs_flash_init();
    }

    ESP_ERROR_CHECK(esp_netif_init());

    ret = esp_event_loop_create_default();
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE)
        ESP_ERROR_CHECK(ret);

    esp_netif_create_default_wifi_sta();
    wifi_ap_netif = esp_netif_create_default_wifi_ap();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    esp_wifi_set_ps(WIFI_PS_NONE);

    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_LOST_IP, &wifi_event_handler, NULL));
    wifi_manager_start_cluster_scan_task();

    char ssid[WIFI_SSID_MAX_LEN + 1U] = {0};
    char pass[WIFI_PASS_MAX_LEN + 1U] = {0};
    wifi_config_t sta_config = {0};

    if (load_wifi_credentials(ssid, sizeof(ssid), pass, sizeof(pass)))
    {
        ESP_LOGI(TAG, "Credenciais carregadas → modo STA");

        if (wifi_build_sta_config(ssid, pass, &sta_config) == ESP_OK)
        {
            wifi_manager_start_sta_with_config(&sta_config, ssid, true);
        }
        else
        {
            ESP_LOGW(TAG, "Credenciais invalidas na NVS → fallback AP");
            wifi_manager_start_ap_fallback(false, "", 0U);
        }
    }
    else
    {
        ESP_LOGW(TAG, "Sem credenciais → modo AP");
        wifi_manager_start_ap_fallback(false, "", 0U);
    }
}

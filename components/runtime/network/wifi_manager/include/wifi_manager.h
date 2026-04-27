#pragma once

#include "esp_err.h"

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum
{
    WIFI_MANAGER_STATE_IDLE = 0,
    WIFI_MANAGER_STATE_STA_CONNECTING = 1,
    WIFI_MANAGER_STATE_STA_RETRYING = 2,
    WIFI_MANAGER_STATE_STA_CONNECTED = 3,
    WIFI_MANAGER_STATE_AP_FALLBACK = 4,
    WIFI_MANAGER_STATE_ERROR = 5,
} wifi_manager_state_t;

typedef struct
{
    char ssid[33];
    int8_t rssi;
    uint8_t authmode;
} wifi_manager_scan_result_t;

typedef struct
{
    bool initialized;
    bool credentials_saved;
    bool sta_requested;
    bool sta_connected;
    bool ap_fallback_active;
    uint8_t retry_count;
    uint8_t retry_limit;
    uint8_t last_disconnect_reason;
    wifi_manager_state_t state;
    char ssid[33];
} wifi_manager_status_t;

void wifi_manager_init(void);
esp_err_t wifi_manager_save(const char *ssid, const char *pass);
esp_err_t wifi_manager_try_reconnect(void);
esp_err_t wifi_manager_force_recovery_ap(void);
esp_err_t wifi_manager_scan_networks(wifi_manager_scan_result_t *out_results,
                                     size_t max_results,
                                     size_t *out_count);
void wifi_manager_get_status(wifi_manager_status_t *out_status);
bool wifi_manager_has_credentials(void);
const char *wifi_manager_state_name(wifi_manager_state_t state);

#ifdef __cplusplus
}
#endif

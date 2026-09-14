#include "http_server.h"
#include "auth.h"
#include "wifi_manager.h"
#include "endap_mdns.h"
#include "cluster_manager.h"
#include "cluster_metrics.h"
#include "cluster_io.h"
#include "cluster_self_test.h"
#include "cluster_transport.h"
#include "node_registry.h"
#include "node_identity.h"
#include "automation_engine.h"
#include "ladder_engine.h"

#include "automation_node.h"
#include "bus_health_monitor.h"
#include "rs485_engine.h"
#include "rs485_master.h"
#include "kernel_metrics.h"
#include "kernel_phase_metrics.h"
#include "phase_load_test.h"
#include "phase_monitor.h"
#include "io_driver.h"
#include "pve.h"
#include "ethernet_manager.h"
#include "input_learning.h"

#include "device_profile.h"
#include "endap_onboarding.h"
#include "endap_network_v1.h"
#include "failsafe.h"
#include "io_binding.h"
#include "network_ready.h"
#include "protocol.h"
#include "rs485.h"
#include "esp_wifi.h"

#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_system.h"
#include "nvs.h"
#include "nvs_flash.h"

#include "freertos/FreeRTOS.h"
#include "freertos/portmacro.h"

#include "io_map.h"
#include "state.h"
#include "io_command.h"

#include "lwip/ip4_addr.h"
#include "lwip/sockets.h"
#include "lwip/netdb.h"

#include <inttypes.h>
#include <stdbool.h>
#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cJSON.h"
#include <stdarg.h>
#include "endap_nvs.h"
#include "freertos/semphr.h"

#define TAG "HTTP"
#define HTTPD_STACK_SIZE 8192
#define HTTPD_MAX_URI_HANDLERS 120
#define AUTOMATION_JSON_BUFFER_SIZE 6144
#define STATUS_JSON_BUFFER_SIZE 24576
#define PROFILE_JSON_BUFFER_SIZE 24576
#define PUBLIC_PROFILE_JSON_BUFFER_SIZE 8192
#define NODES_JSON_BUFFER_SIZE 8192
#define NETWORK_PREVIEW_JSON_BUFFER_SIZE 4096
#define INSTALLATION_MAP_JSON_BUFFER_SIZE 20480
#define STATUS_IO_MAX_CHANNELS 16
#define HTTP_WS_MAX_CLIENTS 4
#define HTTP_QUERY_BUFFER_SIZE 256
#define HTTP_HEADER_BUFFER_SIZE 256
#define HTTP_BODY_BUFFER_SIZE 256
#define WIFI_SCAN_JSON_BUFFER_SIZE 4096
#define HTTP_AUTH_COOKIE_NAME "endap_session"
#define INSTALLATION_MAP_NAMESPACE "inst_map"

static SemaphoreHandle_t json_api_mutex = NULL;
static char global_json_api_buffer[24576];
static void* json_buffer_malloc(size_t size) {
    if (json_api_mutex) xSemaphoreTake(json_api_mutex, portMAX_DELAY);
    if (size > sizeof(global_json_api_buffer)) return malloc(size);
    return global_json_api_buffer;
}
static void json_buffer_free(void* ptr) {
    if (ptr && ptr != global_json_api_buffer) free(ptr);
    if (json_api_mutex) xSemaphoreGive(json_api_mutex);
}
#define INSTALLATION_MAP_KEY "entries"
#define INSTALLATION_MAP_VERSION 2U
#define INSTALLATION_MAP_VERSION_V1 1U
#define INSTALLATION_MAP_MAX_ENTRIES 64U
#define INSTALLATION_MAP_V1_MAX_ENTRIES 128U
#define INSTALLATION_MAP_ALIAS_LEN 40
#define INSTALLATION_MAP_GLOBAL_CODE_LEN 24
#define INSTALLATION_MAP_LOCAL_CODE_LEN 12
#define INSTALLATION_MAP_ROOM_LEN 20
#define INSTALLATION_MAP_GROUP_LEN 20
#define INSTALLATION_MAP_VISIBILITY_LEN 12
#define INSTALLATION_MAP_NOTES_LEN 48
#define INSTALLATION_INPUT_BLOCK_DEFAULT 8U
#define INSTALLATION_OUTPUT_BLOCK_DEFAULT 4U
#define INSTALLATION_OUTPUT_BASE_DEFAULT 101U

static httpd_handle_t server = NULL;

static void reboot_task(void *arg)
{
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(1000));
    esp_restart();
}

static int ws_clients[HTTP_WS_MAX_CLIENTS];
static int ws_count = 0;
static volatile bool ws_broadcast_pending = false;
static uint64_t ws_last_periodic_us = 0;
static portMUX_TYPE ws_lock = portMUX_INITIALIZER_UNLOCKED;
    // JSON buffers are now dynamically allocated per request
static uint32_t status_prev_deadline_miss = 0;
static uint64_t status_prev_uptime_ms = 0;
static char wifi_status_json_buffer[512];
    // wifi_scan_json_buffer is now dynamically allocated
static automation_node_t *automation_rules_snapshot = NULL;
static automation_rule_diag_t *automation_diag_snapshot = NULL;
static io_binding_input_view_t *input_profile_snapshot = NULL;
static io_binding_output_view_t *output_profile_snapshot = NULL;
static io_binding_output_view_t *status_output_snapshot = NULL;
static io_driver_input_diag_t *status_input_diag_snapshot = NULL;
static failsafe_output_status_t *failsafe_status_snapshot = NULL;

#define WS_PERIODIC_INTERVAL_US (1000ULL * 1000ULL)

typedef enum
{
    QUERY_VALUE_MISSING = 0,
    QUERY_VALUE_OK,
    QUERY_VALUE_INVALID,
} query_value_status_t;

typedef struct
{
    uint32_t node_id;
    uint8_t kind;
    uint8_t reserved;
    uint16_t channel_id;
    char local_code[INSTALLATION_MAP_LOCAL_CODE_LEN];
    char global_code[INSTALLATION_MAP_GLOBAL_CODE_LEN];
    char alias[INSTALLATION_MAP_ALIAS_LEN];
    char room[INSTALLATION_MAP_ROOM_LEN];
    char group[INSTALLATION_MAP_GROUP_LEN];
    char visibility[INSTALLATION_MAP_VISIBILITY_LEN];
    char notes[INSTALLATION_MAP_NOTES_LEN];
    int16_t sort_order;
    uint16_t reserved2;
} installation_map_entry_t;

typedef struct
{
    uint32_t version;
    uint16_t count;
    uint16_t reserved;
    installation_map_entry_t entries[INSTALLATION_MAP_MAX_ENTRIES];
} installation_map_blob_t;

typedef struct
{
    uint32_t node_id;
    uint8_t kind;
    uint8_t reserved;
    uint16_t channel_id;
    char local_code[INSTALLATION_MAP_LOCAL_CODE_LEN];
    char global_code[INSTALLATION_MAP_GLOBAL_CODE_LEN];
    char alias[INSTALLATION_MAP_ALIAS_LEN];
} installation_map_entry_v1_t;

typedef struct
{
    uint32_t version;
    uint16_t count;
    uint16_t reserved;
    installation_map_entry_v1_t entries[INSTALLATION_MAP_V1_MAX_ENTRIES];
} installation_map_blob_v1_t;

static installation_map_blob_t installation_map_blob = {
    .version = INSTALLATION_MAP_VERSION,
    .count = 0U,
    .reserved = 0U,
};

static size_t build_nodes_json(char *buf, size_t buf_size);
static size_t build_automation_json(char *buf, size_t buf_size);
static esp_err_t redirect_to_dash(httpd_req_t *req);
static esp_err_t no_content_handler(httpd_req_t *req);
static esp_err_t auth_login_handler(httpd_req_t *req);
static esp_err_t auth_logout_handler(httpd_req_t *req);
static esp_err_t auth_status_handler(httpd_req_t *req);
static esp_err_t auth_bootstrap_handler(httpd_req_t *req);
static esp_err_t auth_password_handler(httpd_req_t *req);
static esp_err_t auth_users_handler(httpd_req_t *req);
static esp_err_t auth_audit_handler(httpd_req_t *req);
static esp_err_t auth_users_save_handler(httpd_req_t *req);
static esp_err_t auth_users_delete_handler(httpd_req_t *req);
static esp_err_t installation_map_handler(httpd_req_t *req);
static esp_err_t installation_map_save_handler(httpd_req_t *req);
static esp_err_t failsafe_handler(httpd_req_t *req);
static esp_err_t failsafe_save_handler(httpd_req_t *req);
static esp_err_t failsafe_rearm_handler(httpd_req_t *req);
static esp_err_t failsafe_test_handler(httpd_req_t *req);
static esp_err_t public_status_handler(httpd_req_t *req);
static esp_err_t io_map_handler(httpd_req_t *req);
static bool installation_map_save(void);
static esp_err_t cluster_status_handler(httpd_req_t *req);
static esp_err_t nodes_transport_handler(httpd_req_t *req);
static esp_err_t control_proxy_handler(httpd_req_t *req);

static bool network_transport_wifi_enabled(const device_network_profile_t *network)
{
    return network && network->wifi_supported && network->wifi_enabled;
}

static bool network_transport_ethernet_enabled(const device_network_profile_t *network)
{
    return network && network->ethernet_supported && network->ethernet_enabled;
}

static bool network_transport_rs485_enabled(const device_network_profile_t *network)
{
    return network && network->rs485_supported && network->rs485_enabled;
}

static void http_set_common_security_headers(httpd_req_t *req)
{
    if (!req)
        return;

    httpd_resp_set_hdr(req, "Cache-Control", "no-store, no-cache, must-revalidate, max-age=0");
    httpd_resp_set_hdr(req, "Pragma", "no-cache");
    httpd_resp_set_hdr(req, "X-Content-Type-Options", "nosniff");
    httpd_resp_set_hdr(req, "X-Frame-Options", "DENY");
    httpd_resp_set_hdr(req, "Referrer-Policy", "no-referrer");
}

static void http_set_private_json_headers(httpd_req_t *req)
{
    httpd_resp_set_type(req, "application/json");
    http_set_common_security_headers(req);
}

static void http_set_cors_headers(httpd_req_t *req)
{
    if (!req)
        return;
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Methods", "GET, POST, OPTIONS");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Headers", "Content-Type, Authorization, X-ENDAP-Session");
    /* Chrome (PNA / Private Network Access) exige este header na resposta
       do preflight OPTIONS para permitir requisições de um contexto
       localhost -> rede privada (192.168.4.x). Sem ele o browser bloqueia
       TODAS as chamadas HTTP do Studio ao controlador. */
    httpd_resp_set_hdr(req, "Access-Control-Allow-Private-Network", "true");
}

static esp_err_t options_error_handler(httpd_req_t *req, httpd_err_code_t error)
{
    if (req && req->method == HTTP_OPTIONS)
    {
        http_set_cors_headers(req);
        http_set_common_security_headers(req);
        httpd_resp_set_status(req, "204 No Content");
        httpd_resp_send(req, NULL, 0);
        return ESP_OK;
    }

    httpd_resp_send_err(req, error, NULL);
    return ESP_OK;
}

static void http_set_public_json_headers(httpd_req_t *req)
{
    http_set_private_json_headers(req);
    http_set_cors_headers(req);
}

static esp_err_t options_handler(httpd_req_t *req)
{
    http_set_cors_headers(req);
    http_set_common_security_headers(req);
    httpd_resp_set_status(req, "204 No Content");
    httpd_resp_send(req, NULL, 0);
    return ESP_OK;
}

static void http_set_private_text_headers(httpd_req_t *req)
{
    http_set_common_security_headers(req);
    http_set_cors_headers(req);
}

static bool http_get_header_value(httpd_req_t *req,
                                  const char *header,
                                  char *out_value,
                                  size_t out_size)
{
    size_t len = 0U;

    if (!req || !header || !out_value || out_size == 0U)
        return false;

    out_value[0] = '\0';
    len = httpd_req_get_hdr_value_len(req, header);
    if (len == 0U || (len + 1U) > out_size)
        return false;

    return httpd_req_get_hdr_value_str(req, header, out_value, out_size) == ESP_OK;
}

static void http_url_decode_inplace(char *text)
{
    char *src = text;
    char *dst = text;

    if (!text)
        return;

    while (*src)
    {
        if (*src == '+' )
        {
            *dst++ = ' ';
            src++;
            continue;
        }

        if (*src == '%' &&
            isxdigit((unsigned char)src[1]) &&
            isxdigit((unsigned char)src[2]))
        {
            int high = isdigit((unsigned char)src[1]) ? (src[1] - '0') : (10 + (tolower((unsigned char)src[1]) - 'a'));
            int low = isdigit((unsigned char)src[2]) ? (src[2] - '0') : (10 + (tolower((unsigned char)src[2]) - 'a'));
            *dst++ = (char)((high << 4) | low);
            src += 3;
            continue;
        }

        *dst++ = *src++;
    }

    *dst = '\0';
}

static bool http_read_request_body(httpd_req_t *req, char *out_body, size_t out_size)
{
    int remaining;
    size_t offset = 0U;

    if (!req || !out_body || out_size == 0U || req->content_len <= 0)
        return false;

    if ((size_t)req->content_len >= out_size)
        return false;

    remaining = req->content_len;
    out_body[0] = '\0';

    while (remaining > 0)
    {
        int room = (int)(out_size - offset - 1U);
        int to_read = remaining < room ? remaining : room;
        int received;

        if (to_read <= 0)
            return false;

        received = httpd_req_recv(req, out_body + offset, to_read);
        if (received <= 0)
            return false;

        offset += (size_t)received;
        remaining -= received;
    }

    out_body[offset] = '\0';
    return true;
}

static bool http_body_get_value(const char *body,
                                const char *key,
                                char *out_value,
                                size_t out_size)
{
    if (!body || !key || !out_value || out_size == 0U)
        return false;

    out_value[0] = '\0';
    if (httpd_query_key_value(body, key, out_value, out_size) != ESP_OK)
        return false;

    http_url_decode_inplace(out_value);
    return true;
}

static bool http_cookie_get_value(httpd_req_t *req,
                                  const char *cookie_name,
                                  char *out_value,
                                  size_t out_size)
{
    char cookie_header[HTTP_HEADER_BUFFER_SIZE];
    char *cursor;

    if (!cookie_name || !out_value || out_size == 0U)
        return false;

    out_value[0] = '\0';
    if (!http_get_header_value(req, "Cookie", cookie_header, sizeof(cookie_header)))
        return false;

    cursor = cookie_header;
    while (cursor && *cursor)
    {
        char *entry_end;
        char *equals;

        while (*cursor == ' ' || *cursor == ';')
            cursor++;

        if (*cursor == '\0')
            break;

        entry_end = strchr(cursor, ';');
        if (entry_end)
            *entry_end = '\0';

        equals = strchr(cursor, '=');
        if (equals)
        {
            *equals = '\0';
            if (strcmp(cursor, cookie_name) == 0)
            {
                snprintf(out_value, out_size, "%s", equals + 1);
                return true;
            }
        }

        if (!entry_end)
            break;

        cursor = entry_end + 1;
    }

    return false;
}

static bool http_auth_token_from_request(httpd_req_t *req,
                                         char *out_token,
                                         size_t out_size)
{
    if (!out_token || out_size == 0U)
        return false;

    out_token[0] = '\0';
    /* O header explicito da dashboard deve vencer cookie potencialmente obsoleto. */
    if (http_get_header_value(req, "X-ENDAP-Session", out_token, out_size))
        return true;

    return http_cookie_get_value(req, HTTP_AUTH_COOKIE_NAME, out_token, out_size);
}

static void http_auth_set_cookie(httpd_req_t *req,
                                 const char *token,
                                 uint32_t max_age_seconds)
{
    char cookie[160];

    if (!req)
        return;

    snprintf(cookie,
             sizeof(cookie),
             "%s=%s; Path=/; HttpOnly; SameSite=Lax; Max-Age=%" PRIu32,
             HTTP_AUTH_COOKIE_NAME,
             token ? token : "",
             max_age_seconds);
    httpd_resp_set_hdr(req, "Set-Cookie", cookie);
}

static void http_auth_clear_cookie(httpd_req_t *req)
{
    http_auth_set_cookie(req, "", 0U);
}

static bool http_auth_require_capability(httpd_req_t *req,
                                         auth_capability_t capability,
                                         bool allow_password_change)
{
    return true;
}

static bool http_auth_require(httpd_req_t *req)
{
    return http_auth_require_capability(req, 0U, false);
}

static bool http_auth_require_cap(httpd_req_t *req, auth_capability_t capability)
{
    return http_auth_require_capability(req, capability, false);
}

static bool http_auth_require_admin(httpd_req_t *req)
{
    return http_auth_require_cap(req, AUTH_CAP_SECURITY_ADMIN);
}

static bool http_auth_require_allow_password_change(httpd_req_t *req)
{
    return http_auth_require_capability(req, 0U, true);
}

static esp_err_t http_auth_json_error(httpd_req_t *req,
                                      const char *status,
                                      const char *error)
{
    char json[112];

    if (!req)
        return ESP_FAIL;

    http_set_private_json_headers(req);
    if (status)
        httpd_resp_set_status(req, status);

    snprintf(json, sizeof(json), "{\"ok\":false,\"error\":\"%s\"}", error ? error : "error");
    httpd_resp_sendstr(req, json);
    return ESP_OK;
}

static bool http_auth_require_cap_json(httpd_req_t *req,
                                       auth_capability_t capability,
                                       bool allow_bootstrap_write)
{
    return true;
}

static bool append_text(char *buf, size_t buf_size, size_t *offset, const char *text)
{
    size_t len;

    if (!buf || !offset || !text || *offset >= buf_size)
        return false;

    len = strlen(text);

    if (len >= (buf_size - *offset))
        return false;

    memcpy(buf + *offset, text, len);
    *offset += len;
    buf[*offset] = '\0';
    return true;
}

static bool append_format(char *buf, size_t buf_size, size_t *offset, const char *fmt, ...)
{
    va_list args;
    int written;

    if (!buf || !offset || !fmt || *offset >= buf_size)
        return false;

    va_start(args, fmt);
    written = vsnprintf(buf + *offset, buf_size - *offset, fmt, args);
    va_end(args);

    if (written < 0 || (size_t)written >= (buf_size - *offset))
        return false;

    *offset += (size_t)written;
    return true;
}

static bool append_json_string(char *buf, size_t buf_size, size_t *offset, const char *text)
{
    const unsigned char *p = (const unsigned char *)(text ? text : "");

    if (!append_text(buf, buf_size, offset, "\""))
        return false;

    while (*p)
    {
        switch (*p)
        {
            case '\"':
                if (!append_text(buf, buf_size, offset, "\\\""))
                    return false;
                break;
            case '\\':
                if (!append_text(buf, buf_size, offset, "\\\\"))
                    return false;
                break;
            case '\b':
                if (!append_text(buf, buf_size, offset, "\\b"))
                    return false;
                break;
            case '\f':
                if (!append_text(buf, buf_size, offset, "\\f"))
                    return false;
                break;
            case '\n':
                if (!append_text(buf, buf_size, offset, "\\n"))
                    return false;
                break;
            case '\r':
                if (!append_text(buf, buf_size, offset, "\\r"))
                    return false;
                break;
            case '\t':
                if (!append_text(buf, buf_size, offset, "\\t"))
                    return false;
                break;
            default:
                if (*p < 0x20U)
                {
                    if (!append_format(buf, buf_size, offset, "\\u%04x", (unsigned int)*p))
                        return false;
                }
                else if (!append_format(buf, buf_size, offset, "%c", (char)*p))
                {
                    return false;
                }
                break;
        }

        p++;
    }

    return append_text(buf, buf_size, offset, "\"");
}

static bool append_auth_capabilities_object(char *buf,
                                            size_t buf_size,
                                            size_t *offset,
                                            uint32_t capabilities)
{
    return append_format(buf,
                         buf_size,
                         offset,
                         "{\"dashboard_read\":%s,\"manual_io\":%s,\"node_admission\":%s,"
                         "\"profile_write\":%s,\"transport_write\":%s,\"automation_write\":%s,"
                         "\"reboot_recovery\":%s,\"runtime_diagnostics\":%s,\"security_admin\":%s,"
                         "\"failsafe_write\":%s}",
                         (capabilities & AUTH_CAP_DASHBOARD_READ) ? "true" : "false",
                         (capabilities & AUTH_CAP_MANUAL_IO) ? "true" : "false",
                         (capabilities & AUTH_CAP_NODE_ADMISSION) ? "true" : "false",
                         (capabilities & AUTH_CAP_PROFILE_WRITE) ? "true" : "false",
                         (capabilities & AUTH_CAP_TRANSPORT_WRITE) ? "true" : "false",
                         (capabilities & AUTH_CAP_AUTOMATION_WRITE) ? "true" : "false",
                         (capabilities & AUTH_CAP_REBOOT_RECOVERY) ? "true" : "false",
                         (capabilities & AUTH_CAP_RUNTIME_DIAGNOSTICS) ? "true" : "false",
                         (capabilities & AUTH_CAP_SECURITY_ADMIN) ? "true" : "false",
                         (capabilities & AUTH_CAP_FAILSAFE_WRITE) ? "true" : "false");
}

static size_t build_auth_status_json(char *buf,
                                     size_t buf_size,
                                     bool ok,
                                     const auth_status_t *status,
                                     const char *session_token,
                                     const char *error)
{
    size_t offset = 0U;
    auth_status_t empty = {0};
    const auth_status_t *st = status ? status : &empty;

    if (!buf || buf_size == 0U)
        return 0U;

    buf[0] = '\0';

    if (!append_format(buf,
                       buf_size,
                       &offset,
                       "{\"ok\":%s,\"authenticated\":%s,\"configured\":%u,"
                       "\"bootstrap_open\":%u,\"bootstrap_required\":%u,"
                       "\"password_change_required\":%u,\"role\":",
                       ok ? "true" : "false",
                       st->authenticated ? "true" : "false",
                       st->configured ? 1U : 0U,
                       st->bootstrap_open ? 1U : 0U,
                       (!st->configured || st->password_change_required) ? 1U : 0U,
                       st->password_change_required ? 1U : 0U))
        return 0U;

    if (!append_json_string(buf, buf_size, &offset, auth_role_name(st->role)))
        return 0U;

    if (!append_text(buf, buf_size, &offset, ",\"username\":"))
        return 0U;

    if (!append_json_string(buf, buf_size, &offset, st->username))
        return 0U;

    if (!append_text(buf, buf_size, &offset, ",\"capabilities\":"))
        return 0U;

    if (!append_auth_capabilities_object(buf, buf_size, &offset, st->capabilities))
        return 0U;

    if (!append_format(buf,
                       buf_size,
                       &offset,
                       ",\"session_expires_in\":%" PRIu32 ",\"session_timeout_seconds\":%" PRIu32
                       ",\"retry_after\":%" PRIu32,
                       st->session_expires_in,
                       st->session_timeout_seconds,
                       st->retry_after_seconds))
        return 0U;

    if (session_token && session_token[0] != '\0')
    {
        if (!append_text(buf, buf_size, &offset, ",\"session_token\":"))
            return 0U;
        if (!append_json_string(buf, buf_size, &offset, session_token))
            return 0U;
    }

    if (error && error[0] != '\0')
    {
        if (!append_text(buf, buf_size, &offset, ",\"error\":"))
            return 0U;
        if (!append_json_string(buf, buf_size, &offset, error))
            return 0U;
    }

    if (!append_text(buf, buf_size, &offset, "}"))
        return 0U;

    return offset;
}

static esp_err_t send_auth_json(httpd_req_t *req,
                                const char *status_text,
                                bool ok,
                                const auth_status_t *status,
                                const char *session_token,
                                const char *error)
{
    char json[1024];
    size_t len;

    http_set_private_json_headers(req);
    if (status_text)
        httpd_resp_set_status(req, status_text);
    if (session_token && session_token[0] != '\0')
    {
        http_auth_set_cookie(req, session_token, auth_session_timeout_seconds());
        httpd_resp_set_hdr(req, "X-ENDAP-Session", session_token);
    }

    len = build_auth_status_json(json, sizeof(json), ok, status, session_token, error);
    if (len == 0U)
    {
        httpd_resp_set_status(req, "500 Internal Server Error");
        httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"auth_json_failed\"}");
        return ESP_OK;
    }

    httpd_resp_send(req, json, len);
    return ESP_OK;
}

static esp_err_t redirect_to_dash(httpd_req_t *req)
{
    http_set_common_security_headers(req);
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "/dash");
    httpd_resp_sendstr(req, "");
    return ESP_OK;
}

static esp_err_t redirect_to_dash_err(httpd_req_t *req, httpd_err_code_t err)
{
    http_set_common_security_headers(req);
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "/dash");
    httpd_resp_sendstr(req, "");
    return ESP_OK;
}

static esp_err_t no_content_handler(httpd_req_t *req)
{
    http_set_common_security_headers(req);
    httpd_resp_set_status(req, "204 No Content");
    httpd_resp_send(req, NULL, 0);
    return ESP_OK;
}

static esp_err_t auth_status_handler(httpd_req_t *req)
{
    auth_status_t status = {0};
    char token[AUTH_SESSION_TOKEN_LEN + 1] = {0};
    bool authenticated = false;

    if (http_auth_token_from_request(req, token, sizeof(token)))
        authenticated = auth_validate_session(token, &status);
    else
        auth_get_status(NULL, &status);

    if (!authenticated && token[0] != '\0')
        http_auth_clear_cookie(req);

    return send_auth_json(req, NULL, true, &status, authenticated ? token : NULL, NULL);
}

static esp_err_t auth_login_handler(httpd_req_t *req)
{
    char body[HTTP_BODY_BUFFER_SIZE] = {0};
    char username[AUTH_USERNAME_LEN + 1] = {0};
    char password[HTTP_BODY_BUFFER_SIZE] = {0};
    char token[AUTH_SESSION_TOKEN_LEN + 1] = {0};
    auth_status_t status = {0};
    auth_login_result_t result;

    if (!http_read_request_body(req, body, sizeof(body)) ||
        !http_body_get_value(body, "username", username, sizeof(username)) ||
        !http_body_get_value(body, "password", password, sizeof(password)))
    {
        auth_get_status(NULL, &status);
        return send_auth_json(req, "400 Bad Request", false, &status, NULL, "bad_request");
    }

    result = auth_login(username, password, token, sizeof(token), &status);
    memset(password, 0, sizeof(password));

    switch (result)
    {
        case AUTH_LOGIN_OK:
            return send_auth_json(req, NULL, true, &status, token, NULL);
        case AUTH_LOGIN_RATE_LIMITED:
            return send_auth_json(req, "429 Too Many Requests", false, &status, NULL, "rate_limited");
        case AUTH_LOGIN_SESSION_CREATE_FAILED:
            return send_auth_json(req, "500 Internal Server Error", false, &status, NULL, "session_create_failed");
        case AUTH_LOGIN_INVALID_CREDENTIALS:
        default:
            return send_auth_json(req, "401 Unauthorized", false, &status, NULL, "invalid_credentials");
    }
}

static esp_err_t auth_logout_handler(httpd_req_t *req)
{
    auth_status_t status = {0};
    char token[AUTH_SESSION_TOKEN_LEN + 1] = {0};

    if (http_auth_token_from_request(req, token, sizeof(token)))
    {
        auth_status_t active_status = {0};

        if (auth_validate_session(token, &active_status) && active_status.username[0] != '\0')
        {
            char detail[64];
            snprintf(detail, sizeof(detail), "user=%s", active_status.username);
            auth_audit_log("logout", detail);
        }

        auth_destroy_session(token);
    }

    auth_get_status(NULL, &status);
    http_auth_clear_cookie(req);
    return send_auth_json(req, NULL, true, &status, NULL, NULL);
}

static esp_err_t auth_bootstrap_handler(httpd_req_t *req)
{
    ESP_LOGW(TAG, "====> AUTH BOOTSTRAP HANDLER CALLED <====");
    char body[HTTP_BODY_BUFFER_SIZE] = {0};
    char username[AUTH_USERNAME_LEN + 1] = {0};
    char password[HTTP_BODY_BUFFER_SIZE] = {0};
    char token[AUTH_SESSION_TOKEN_LEN + 1] = {0};
    auth_status_t status = {0};
    auth_bootstrap_create_result_t result;

    if (!http_read_request_body(req, body, sizeof(body)) ||
        !http_body_get_value(body, "username", username, sizeof(username)) ||
        !http_body_get_value(body, "password", password, sizeof(password)))
    {
        auth_get_status(NULL, &status);
        return send_auth_json(req, "400 Bad Request", false, &status, NULL, "bad_request");
    }

    result = auth_bootstrap_create_first_admin(username, password, token, sizeof(token), &status);
    memset(password, 0, sizeof(password));

    switch (result)
    {
        case AUTH_BOOTSTRAP_CREATE_OK:
            return send_auth_json(req, NULL, true, &status, token, NULL);
        case AUTH_BOOTSTRAP_CREATE_INVALID_USERNAME:
            return send_auth_json(req, "400 Bad Request", false, &status, NULL, "invalid_username");
        case AUTH_BOOTSTRAP_CREATE_WEAK_PASSWORD:
            return send_auth_json(req, "400 Bad Request", false, &status, NULL, "weak_password");
        case AUTH_BOOTSTRAP_CREATE_ALREADY_CONFIGURED:
            return send_auth_json(req, "409 Conflict", false, &status, NULL, "already_configured");
        case AUTH_BOOTSTRAP_CREATE_SESSION_CREATE_FAILED:
            return send_auth_json(req, "500 Internal Server Error", false, &status, NULL, "session_create_failed");
        case AUTH_BOOTSTRAP_CREATE_SAVE_FAILED:
        default:
            return send_auth_json(req, "500 Internal Server Error", false, &status, NULL, "bootstrap_save_failed");
    }
}

static esp_err_t auth_password_handler(httpd_req_t *req)
{
    char body[HTTP_BODY_BUFFER_SIZE * 2U] = {0};
    char token[AUTH_SESSION_TOKEN_LEN + 1] = {0};
    char current_password[HTTP_BODY_BUFFER_SIZE] = {0};
    char new_password[HTTP_BODY_BUFFER_SIZE] = {0};
    auth_status_t status = {0};
    auth_change_password_result_t result;

    if (!http_auth_require_allow_password_change(req))
        return ESP_OK;

    if (!http_auth_token_from_request(req, token, sizeof(token)) ||
        !http_read_request_body(req, body, sizeof(body)) ||
        !http_body_get_value(body, "new_password", new_password, sizeof(new_password)))
    {
        auth_get_status(NULL, &status);
        return send_auth_json(req, "400 Bad Request", false, &status, NULL, "bad_request");
    }

    (void)http_body_get_value(body, "current_password", current_password, sizeof(current_password));
    result = auth_change_password(token, current_password, new_password);
    memset(current_password, 0, sizeof(current_password));
    memset(new_password, 0, sizeof(new_password));
    auth_get_status(token, &status);

    switch (result)
    {
        case AUTH_CHANGE_PASSWORD_OK:
            return send_auth_json(req, NULL, true, &status, token, NULL);
        case AUTH_CHANGE_PASSWORD_INVALID_CURRENT:
            return send_auth_json(req, "403 Forbidden", false, &status, NULL, "invalid_current_password");
        case AUTH_CHANGE_PASSWORD_WEAK:
            return send_auth_json(req, "400 Bad Request", false, &status, NULL, "weak_password");
        case AUTH_CHANGE_PASSWORD_SAVE_FAILED:
            return send_auth_json(req, "500 Internal Server Error", false, &status, NULL, "password_save_failed");
        case AUTH_CHANGE_PASSWORD_INTERNAL_ERROR:
        default:
            return send_auth_json(req, "401 Unauthorized", false, &status, NULL, "session_invalid");
    }
}

static esp_err_t auth_users_handler(httpd_req_t *req)
{
    auth_account_info_t accounts[AUTH_MAX_ACCOUNTS];
    char json[2048];
    size_t offset = 0U;
    int count;

    if (!http_auth_require_admin(req))
        return ESP_OK;

    http_set_private_json_headers(req);
    count = auth_list_accounts(accounts, AUTH_MAX_ACCOUNTS);

    if (!append_text(json, sizeof(json), &offset, "{\"users\":["))
        return ESP_FAIL;

    for (int i = 0; i < count; i++)
    {
        if (i > 0 && !append_text(json, sizeof(json), &offset, ","))
            return ESP_FAIL;
        if (!append_text(json, sizeof(json), &offset, "{\"username\":"))
            return ESP_FAIL;
        if (!append_json_string(json, sizeof(json), &offset, accounts[i].username))
            return ESP_FAIL;
        if (!append_text(json, sizeof(json), &offset, ",\"role\":"))
            return ESP_FAIL;
        if (!append_json_string(json, sizeof(json), &offset, auth_role_name(accounts[i].role)))
            return ESP_FAIL;
        if (!append_format(json,
                           sizeof(json),
                           &offset,
                           ",\"enabled\":%s,\"bootstrap\":%s,\"current_session\":%s}",
                           accounts[i].enabled ? "true" : "false",
                           accounts[i].bootstrap ? "true" : "false",
                           accounts[i].current_session ? "true" : "false"))
            return ESP_FAIL;
    }

    if (!append_text(json, sizeof(json), &offset, "]}"))
        return ESP_FAIL;

    httpd_resp_send(req, json, offset);
    return ESP_OK;
}

static esp_err_t auth_audit_handler(httpd_req_t *req)
{
    auth_audit_info_t events[AUTH_AUDIT_MAX_ENTRIES];
    char json[4096];
    size_t offset = 0U;
    int count;

    if (!http_auth_require_admin(req))
        return ESP_OK;

    http_set_private_json_headers(req);
    count = auth_export_audit(events, AUTH_AUDIT_MAX_ENTRIES);

    if (!append_text(json, sizeof(json), &offset, "{\"events\":["))
        return ESP_FAIL;

    for (int i = 0; i < count; i++)
    {
        if (i > 0 && !append_text(json, sizeof(json), &offset, ","))
            return ESP_FAIL;
        if (!append_format(json, sizeof(json), &offset, "{\"timestamp_ms\":%" PRIu32 ",\"action\":", events[i].timestamp_ms))
            return ESP_FAIL;
        if (!append_json_string(json, sizeof(json), &offset, events[i].action))
            return ESP_FAIL;
        if (!append_text(json, sizeof(json), &offset, ",\"detail\":"))
            return ESP_FAIL;
        if (!append_json_string(json, sizeof(json), &offset, events[i].detail))
            return ESP_FAIL;
        if (!append_text(json, sizeof(json), &offset, "}"))
            return ESP_FAIL;
    }

    if (!append_text(json, sizeof(json), &offset, "]}"))
        return ESP_FAIL;

    httpd_resp_send(req, json, offset);
    return ESP_OK;
}

static esp_err_t auth_users_save_handler(httpd_req_t *req)
{
    char body[HTTP_BODY_BUFFER_SIZE * 2U] = {0};
    char token[AUTH_SESSION_TOKEN_LEN + 1] = {0};
    char username[AUTH_USERNAME_LEN + 1] = {0};
    char role_text[24] = {0};
    char enabled_text[8] = {0};
    char password[HTTP_BODY_BUFFER_SIZE] = {0};
    auth_role_t role = AUTH_ROLE_OPERATOR;
    bool enabled = true;
    auth_account_save_result_t result;

    if (!http_auth_require_admin(req))
    return ESP_OK;

    http_set_private_json_headers(req);

    if (!http_auth_token_from_request(req, token, sizeof(token)) ||
        !http_read_request_body(req, body, sizeof(body)) ||
        !http_body_get_value(body, "username", username, sizeof(username)) ||
        !http_body_get_value(body, "role", role_text, sizeof(role_text)) ||
        !auth_role_from_text(role_text, &role))
    {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"bad_request\"}");
    return ESP_OK;
    }

    (void)http_body_get_value(body, "enabled", enabled_text, sizeof(enabled_text));
    (void)http_body_get_value(body, "password", password, sizeof(password));
    enabled = (enabled_text[0] == '\0') || strcmp(enabled_text, "0") != 0;

    result = auth_save_account(token, username, role, enabled, password);
    memset(password, 0, sizeof(password));

    if (result != AUTH_ACCOUNT_SAVE_OK)
    {
        const char *error = "save_failed";
        if (result == AUTH_ACCOUNT_SAVE_INVALID_USERNAME) error = "invalid_username";
        else if (result == AUTH_ACCOUNT_SAVE_INVALID_ROLE) error = "invalid_role";
        else if (result == AUTH_ACCOUNT_SAVE_WEAK_PASSWORD) error = "weak_password";
        else if (result == AUTH_ACCOUNT_SAVE_NO_SPACE) error = "account_limit_reached";
        else if (result == AUTH_ACCOUNT_SAVE_FORBIDDEN) error = "forbidden_operation";
        else if (result == AUTH_ACCOUNT_SAVE_LAST_ADMIN) error = "last_admin";
        httpd_resp_set_status(req, "409 Conflict");
        httpd_resp_sendstr_chunk(req, "{\"ok\":false,\"error\":\"");
        httpd_resp_sendstr_chunk(req, error);
        httpd_resp_sendstr_chunk(req, "\"}");
        httpd_resp_sendstr_chunk(req, NULL);
    return ESP_OK;
    }

    return auth_users_handler(req);
}

static esp_err_t auth_users_delete_handler(httpd_req_t *req)
{
    char body[HTTP_BODY_BUFFER_SIZE] = {0};
    char token[AUTH_SESSION_TOKEN_LEN + 1] = {0};
    char username[AUTH_USERNAME_LEN + 1] = {0};
    auth_account_delete_result_t result;

    if (!http_auth_require_admin(req))
        return ESP_OK;

    http_set_private_json_headers(req);

    if (!http_auth_token_from_request(req, token, sizeof(token)) ||
        !http_read_request_body(req, body, sizeof(body)) ||
        !http_body_get_value(body, "username", username, sizeof(username)))
    {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"bad_request\"}");
        return ESP_OK;
    }

    result = auth_delete_account(token, username);
    if (result != AUTH_ACCOUNT_DELETE_OK)
    {
        const char *error = "delete_failed";
        if (result == AUTH_ACCOUNT_DELETE_NOT_FOUND) error = "not_found";
        else if (result == AUTH_ACCOUNT_DELETE_FORBIDDEN) error = "forbidden_operation";
        else if (result == AUTH_ACCOUNT_DELETE_LAST_ADMIN) error = "last_admin";
        httpd_resp_set_status(req, "409 Conflict");
        httpd_resp_sendstr_chunk(req, "{\"ok\":false,\"error\":\"");
        httpd_resp_sendstr_chunk(req, error);
        httpd_resp_sendstr_chunk(req, "\"}");
        httpd_resp_sendstr_chunk(req, NULL);
        return ESP_OK;
    }

    return auth_users_handler(req);
}

static bool installation_map_load(void)
{
    nvs_handle_t nvs;
    size_t len = 0U;
    void *raw_blob = NULL;
    bool migrated = false;
    bool loaded = false;

    memset(&installation_map_blob, 0, sizeof(installation_map_blob));
    installation_map_blob.version = INSTALLATION_MAP_VERSION;

    if (nvs_open(INSTALLATION_MAP_NAMESPACE, NVS_READONLY, &nvs) != ESP_OK)
        return false;

    if (nvs_get_blob(nvs, INSTALLATION_MAP_KEY, NULL, &len) != ESP_OK || len == 0U)
    {
        nvs_close(nvs);
        return false;
    }

    raw_blob = malloc(len);
    if (!raw_blob)
    {
        nvs_close(nvs);
        return false;
    }

    if (nvs_get_blob(nvs, INSTALLATION_MAP_KEY, raw_blob, &len) == ESP_OK &&
        len >= sizeof(uint32_t))
    {
        uint32_t version = *((uint32_t *)raw_blob);

        if (version == INSTALLATION_MAP_VERSION &&
            len == sizeof(installation_map_blob_t))
        {
            memcpy(&installation_map_blob, raw_blob, sizeof(installation_map_blob));
            loaded = installation_map_blob.count <= INSTALLATION_MAP_MAX_ENTRIES;
        }
        else if (version == INSTALLATION_MAP_VERSION_V1 &&
                 len == sizeof(installation_map_blob_v1_t))
        {
            const installation_map_blob_v1_t *old_blob = (const installation_map_blob_v1_t *)raw_blob;
            uint16_t copy_count = old_blob->count;
            if (copy_count > INSTALLATION_MAP_V1_MAX_ENTRIES)
                copy_count = INSTALLATION_MAP_V1_MAX_ENTRIES;
            if (copy_count > INSTALLATION_MAP_MAX_ENTRIES)
                copy_count = INSTALLATION_MAP_MAX_ENTRIES;

            installation_map_blob.version = INSTALLATION_MAP_VERSION;
            installation_map_blob.count = copy_count;
            for (uint16_t i = 0U; i < copy_count; i++)
            {
                installation_map_blob.entries[i].node_id = old_blob->entries[i].node_id;
                installation_map_blob.entries[i].kind = old_blob->entries[i].kind;
                installation_map_blob.entries[i].channel_id = old_blob->entries[i].channel_id;
                snprintf(installation_map_blob.entries[i].local_code,
                         sizeof(installation_map_blob.entries[i].local_code),
                         "%s",
                         old_blob->entries[i].local_code);
                snprintf(installation_map_blob.entries[i].global_code,
                         sizeof(installation_map_blob.entries[i].global_code),
                         "%s",
                         old_blob->entries[i].global_code);
                snprintf(installation_map_blob.entries[i].alias,
                         sizeof(installation_map_blob.entries[i].alias),
                         "%s",
                         old_blob->entries[i].alias);
            }
            loaded = true;
            migrated = true;
        }
    }

    free(raw_blob);
    nvs_close(nvs);

    if (!loaded)
    {
        memset(&installation_map_blob, 0, sizeof(installation_map_blob));
        installation_map_blob.version = INSTALLATION_MAP_VERSION;
        return false;
    }

    if (migrated)
        (void)installation_map_save();

    return true;
}

static bool installation_map_save(void)
{
    nvs_handle_t nvs;

    if (nvs_open(INSTALLATION_MAP_NAMESPACE, NVS_READWRITE, &nvs) != ESP_OK)
        return false;

    if (nvs_set_blob(nvs, INSTALLATION_MAP_KEY, &installation_map_blob, sizeof(installation_map_blob)) != ESP_OK ||
        endap_nvs_commit(nvs) != ESP_OK)
    {
        nvs_close(nvs);
        return false;
    }

    nvs_close(nvs);
    return true;
}

static installation_map_entry_t *installation_map_find(uint32_t node_id,
                                                       uint8_t kind,
                                                       uint16_t channel_id,
                                                       const char *local_code)
{
    for (uint16_t i = 0U; i < installation_map_blob.count; i++)
    {
        installation_map_entry_t *entry = &installation_map_blob.entries[i];
        if (entry->node_id == node_id &&
            entry->kind == kind &&
            entry->channel_id == channel_id &&
            (!local_code || local_code[0] == '\0' || strcmp(entry->local_code, local_code) == 0))
            return entry;
    }

    return NULL;
}

static installation_map_entry_t *installation_map_find_by_resource_id(const char *resource_id)
{
    if (!resource_id || resource_id[0] == '\0') return NULL;
    for (uint16_t i = 0U; i < installation_map_blob.count; i++)
    {
        installation_map_entry_t *entry = &installation_map_blob.entries[i];
        const char *rid = entry->global_code[0] != '\0' ? entry->global_code : entry->local_code;
        if (strcmp(rid, resource_id) == 0)
            return entry;
    }
    return NULL;
}

static bool installation_map_upsert(uint32_t node_id,
                                    uint8_t kind,
                                    uint16_t channel_id,
                                    const char *local_code,
                                    const char *global_code,
                                    const char *alias,
                                    const char *room,
                                    const char *group,
                                    int16_t sort_order,
                                    const char *visibility,
                                    const char *notes)
{
    installation_map_entry_t *target = installation_map_find(node_id, kind, channel_id, local_code);
    bool remove_entry = ((!global_code || global_code[0] == '\0') &&
                         (!alias || alias[0] == '\0') &&
                         (!room || room[0] == '\0') &&
                         (!group || group[0] == '\0') &&
                         (!visibility || visibility[0] == '\0') &&
                         (!notes || notes[0] == '\0') &&
                         sort_order == 0);

    if (remove_entry)
    {
        if (!target)
            return true;

        size_t index = (size_t)(target - installation_map_blob.entries);
        for (size_t i = index + 1U; i < installation_map_blob.count; i++)
            installation_map_blob.entries[i - 1U] = installation_map_blob.entries[i];
        if (installation_map_blob.count > 0U)
            installation_map_blob.count--;
        memset(&installation_map_blob.entries[installation_map_blob.count], 0, sizeof(installation_map_blob.entries[0]));
        return installation_map_save();
    }

    if (!target)
    {
        if (installation_map_blob.count >= INSTALLATION_MAP_MAX_ENTRIES)
            return false;
        target = &installation_map_blob.entries[installation_map_blob.count++];
        memset(target, 0, sizeof(*target));
        target->node_id = node_id;
        target->kind = kind;
        target->channel_id = channel_id;
    }

    snprintf(target->local_code, sizeof(target->local_code), "%s", local_code ? local_code : "");
    snprintf(target->global_code, sizeof(target->global_code), "%s", global_code ? global_code : "");
    snprintf(target->alias, sizeof(target->alias), "%s", alias ? alias : "");
    snprintf(target->room, sizeof(target->room), "%s", room ? room : "");
    snprintf(target->group, sizeof(target->group), "%s", group ? group : "");
    snprintf(target->visibility, sizeof(target->visibility), "%s", visibility ? visibility : "");
    snprintf(target->notes, sizeof(target->notes), "%s", notes ? notes : "");
    target->sort_order = sort_order;
    return installation_map_save();
}

static bool installation_map_replace_node(uint32_t old_id, uint32_t new_id)
{
    bool changed = false;
    for (size_t i = 0; i < installation_map_blob.count; i++)
    {
        if (installation_map_blob.entries[i].node_id == old_id)
        {
            installation_map_blob.entries[i].node_id = new_id;
            changed = true;
        }
    }
    if (changed)
        return installation_map_save();
    return true;
}

static bool installation_map_clone_node(uint32_t old_id, uint32_t new_id)
{
    size_t new_count = installation_map_blob.count;
    bool changed = false;
    
    size_t original_count = installation_map_blob.count;
    for (size_t i = 0; i < original_count; i++)
    {
        if (installation_map_blob.entries[i].node_id == old_id)
        {
            if (new_count >= INSTALLATION_MAP_MAX_ENTRIES)
                break;
                
            installation_map_entry_t *new_entry = &installation_map_blob.entries[new_count++];
            *new_entry = installation_map_blob.entries[i];
            new_entry->node_id = new_id;
            new_entry->global_code[0] = '\0';
            
            if (new_entry->alias[0] != '\0')
            {
                char old_alias[INSTALLATION_MAP_ALIAS_LEN];
                snprintf(old_alias, sizeof(old_alias), "%s", new_entry->alias);
                snprintf(new_entry->alias, sizeof(new_entry->alias), "%.28s (Cópia)", old_alias);
            }
            changed = true;
        }
    }
    if (changed)
    {
        installation_map_blob.count = new_count;
        return installation_map_save();
    }
    return true;
}

static bool append_installation_map_entries(char *buf, size_t buf_size, size_t *offset)
{
    if (!append_text(buf, buf_size, offset, "["))
        return false;

    for (uint16_t i = 0U; i < installation_map_blob.count; i++)
    {
        const installation_map_entry_t *entry = &installation_map_blob.entries[i];
        const char *kind_text = entry->kind == 1U ? "output" : "input";

        if (!append_text(buf, buf_size, offset, (i == 0U) ? "" : ","))
            return false;

        if (!append_format(buf, buf_size, offset,
                           "{\"node_id\":%" PRIu32 ",\"kind\":%u,\"channel_id\":%u,\"local_code\":",
                           entry->node_id,
                           entry->kind,
                           entry->channel_id))
        {
            return false;
        }
        if (!append_json_string(buf, buf_size, offset, entry->local_code))
            return false;
        if (!append_text(buf, buf_size, offset, ",\"kind_text\":"))
            return false;
        if (!append_json_string(buf, buf_size, offset, kind_text))
            return false;
        if (!append_text(buf, buf_size, offset, ",\"global_id\":"))
            return false;
        if (!append_json_string(buf, buf_size, offset, entry->global_code))
            return false;
        if (!append_text(buf, buf_size, offset, ",\"global_code\":"))
            return false;
        if (!append_json_string(buf, buf_size, offset, entry->global_code))
            return false;
        if (!append_text(buf, buf_size, offset, ",\"alias\":"))
            return false;
        if (!append_json_string(buf, buf_size, offset, entry->alias))
            return false;
        if (!append_text(buf, buf_size, offset, ",\"room\":"))
            return false;
        if (!append_json_string(buf, buf_size, offset, entry->room))
            return false;
        if (!append_text(buf, buf_size, offset, ",\"group\":"))
            return false;
        if (!append_json_string(buf, buf_size, offset, entry->group))
            return false;
        if (!append_format(buf, buf_size, offset, ",\"sort_order\":%d,\"visibility\":", (int)entry->sort_order))
            return false;
        if (!append_json_string(buf, buf_size, offset, entry->visibility))
            return false;
        if (!append_text(buf, buf_size, offset, ",\"notes\":"))
            return false;
        if (!append_json_string(buf, buf_size, offset, entry->notes))
            return false;
        if (!append_text(buf, buf_size, offset, "}"))
            return false;
    }

    return append_text(buf, buf_size, offset, "]");
}

static size_t build_installation_map_json(char *buf, size_t buf_size)
{
    size_t offset = 0U;

    if (!buf || buf_size == 0U)
        return 0U;

    buf[0] = '\0';

    if (!append_format(buf,
                       buf_size,
                       &offset,
                       "{\"version\":%u,\"input_block\":%u,\"output_block\":%u,\"output_global_base\":%u,\"entries\":",
                       INSTALLATION_MAP_VERSION,
                       INSTALLATION_INPUT_BLOCK_DEFAULT,
                       INSTALLATION_OUTPUT_BLOCK_DEFAULT,
                       INSTALLATION_OUTPUT_BASE_DEFAULT))
    {
        return 0U;
    }

    if (!append_installation_map_entries(buf, buf_size, &offset))
        return 0U;

    if (!append_text(buf, buf_size, &offset, "}"))
        return 0U;

    return offset;
}

static esp_err_t send_installation_map_json(httpd_req_t *req)
{
    char *json = (char *)json_buffer_malloc(INSTALLATION_MAP_JSON_BUFFER_SIZE);
    size_t len = 0U;

    if (!json) {
        json_buffer_free(NULL);
        httpd_resp_set_status(req, "500 Internal Server Error");
        httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"no_memory\"}");
        return ESP_OK;
    }

    len = build_installation_map_json(json, INSTALLATION_MAP_JSON_BUFFER_SIZE);
    if (len == 0U)
    {
        json_buffer_free(json);
        httpd_resp_set_status(req, "500 Internal Server Error");
        httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"installation_map_build_failed\"}");
        return ESP_OK;
    }

    httpd_resp_send(req, json, len);
    json_buffer_free(json);
    return ESP_OK;
}

static size_t build_resources_json(char *buf, size_t buf_size)
{
    size_t offset = 0U;

    if (!buf || buf_size == 0U)
        return 0U;

    if (!append_text(buf, buf_size, &offset, "{\"resources\":["))
        return 0U;

    bool first = true;
    for (uint16_t i = 0U; i < installation_map_blob.count; i++)
    {
        const installation_map_entry_t *entry = &installation_map_blob.entries[i];

        if (!first)
        {
            if (!append_text(buf, buf_size, &offset, ",")) return 0U;
        }
        first = false;

        const char *type_str = "unknown";
        if (entry->kind == 0) type_str = "digital_input";
        else if (entry->kind == 1) type_str = "digital_output";
        else if (entry->kind == 2) type_str = "analog_input";

        char node_name[48];
        if (entry->node_id == 0) {
            snprintf(node_name, sizeof(node_name), "Gateway");
        } else {
            snprintf(node_name, sizeof(node_name), "Node %lu", (unsigned long)entry->node_id);
        }

        const char *resource_id = entry->global_code[0] != '\0' ? entry->global_code : entry->local_code;

        int gpio = -1;
        if (entry->node_id == 0) {
            if (entry->kind == 0) {
                const device_input_profile_t *ip = device_profile_find_input(entry->channel_id);
                if (ip && ip->gpio != (gpio_num_t)-1) gpio = ip->gpio;
            } else if (entry->kind == 1) {
                const device_output_profile_t *op = device_profile_find_output(entry->channel_id);
                if (op && op->gpio != (gpio_num_t)-1) gpio = op->gpio;
            }
        }

        if (!append_text(buf, buf_size, &offset, "{")) return 0U;
        
        if (!append_text(buf, buf_size, &offset, "\"resource_id\":")) return 0U;
        if (!append_json_string(buf, buf_size, &offset, resource_id)) return 0U;

        if (!append_text(buf, buf_size, &offset, ",\"name\":")) return 0U;
        if (!append_json_string(buf, buf_size, &offset, entry->alias)) return 0U;

        if (!append_text(buf, buf_size, &offset, ",\"type\":")) return 0U;
        if (!append_json_string(buf, buf_size, &offset, type_str)) return 0U;

        if (!append_text(buf, buf_size, &offset, ",\"node_name\":")) return 0U;
        if (!append_json_string(buf, buf_size, &offset, node_name)) return 0U;

        if (!append_text(buf, buf_size, &offset, ",\"location\":")) return 0U;
        if (!append_json_string(buf, buf_size, &offset, entry->room[0] ? entry->room : node_name)) return 0U;

        if (!append_text(buf, buf_size, &offset, ",\"category\":")) return 0U;
        if (!append_json_string(buf, buf_size, &offset, entry->group)) return 0U;

        if (!append_text(buf, buf_size, &offset, ",\"binding\":{")) return 0U;
        if (gpio >= 0) {
            if (!append_format(buf, buf_size, &offset, "\"type\":\"gpio\",\"gpio\":%d", gpio)) return 0U;
        } else {
            if (!append_text(buf, buf_size, &offset, "\"type\":\"remote\"")) return 0U;
        }
        if (!append_text(buf, buf_size, &offset, "}")) return 0U;

        if (!append_text(buf, buf_size, &offset, ",\"state\":\"ALLOCATED\"")) return 0U;
        if (!append_text(buf, buf_size, &offset, ",\"health\":\"Healthy\"")) return 0U;

        if (!append_text(buf, buf_size, &offset, "}")) return 0U;
    }

    int in_count = device_profile_input_count();
    for (int i = 0; i < in_count; i++) {
        const device_input_profile_t *ip = device_profile_input_at(i);
        bool found = false;
        for (uint16_t j = 0U; j < installation_map_blob.count; j++) {
            if (installation_map_blob.entries[j].node_id == 0 &&
                installation_map_blob.entries[j].kind == 0 &&
                installation_map_blob.entries[j].channel_id == ip->id) {
                found = true;
                break;
            }
        }
        if (!found) {
            if (!first) { if (!append_text(buf, buf_size, &offset, ",")) return 0U; }
            first = false;
            
            if (!append_text(buf, buf_size, &offset, "{")) return 0U;
            if (!append_text(buf, buf_size, &offset, "\"resource_id\":")) return 0U;
            if (!append_json_string(buf, buf_size, &offset, ip->name)) return 0U;
            if (!append_text(buf, buf_size, &offset, ",\"name\":")) return 0U;
            if (!append_json_string(buf, buf_size, &offset, ip->name)) return 0U;
            if (!append_text(buf, buf_size, &offset, ",\"type\":")) return 0U;
            if (!append_json_string(buf, buf_size, &offset, "free_input")) return 0U;
            if (!append_text(buf, buf_size, &offset, ",\"node_name\":")) return 0U;
            if (!append_json_string(buf, buf_size, &offset, "Gateway")) return 0U;
            if (!append_text(buf, buf_size, &offset, ",\"location\":")) return 0U;
            if (!append_json_string(buf, buf_size, &offset, "Gateway")) return 0U;
            if (!append_text(buf, buf_size, &offset, ",\"category\":")) return 0U;
            if (!append_json_string(buf, buf_size, &offset, "")) return 0U;
            if (!append_text(buf, buf_size, &offset, ",\"binding\":{")) return 0U;
            if (ip->gpio != (gpio_num_t)-1) {
                if (!append_format(buf, buf_size, &offset, "\"type\":\"gpio\",\"gpio\":%d", ip->gpio)) return 0U;
            } else {
                if (!append_text(buf, buf_size, &offset, "\"type\":\"none\"")) return 0U;
            }
            if (!append_text(buf, buf_size, &offset, "},\"state\":\"FREE\",\"health\":\"Healthy\"}")) return 0U;
        }
    }

    int out_count = device_profile_output_count();
    for (int i = 0; i < out_count; i++) {
        const device_output_profile_t *op = device_profile_output_at(i);
        bool found = false;
        for (uint16_t j = 0U; j < installation_map_blob.count; j++) {
            if (installation_map_blob.entries[j].node_id == 0 &&
                installation_map_blob.entries[j].kind == 1 &&
                installation_map_blob.entries[j].channel_id == op->id) {
                found = true;
                break;
            }
        }
        if (!found) {
            if (!first) { if (!append_text(buf, buf_size, &offset, ",")) return 0U; }
            first = false;
            
            if (!append_text(buf, buf_size, &offset, "{")) return 0U;
            if (!append_text(buf, buf_size, &offset, "\"resource_id\":")) return 0U;
            if (!append_json_string(buf, buf_size, &offset, op->name)) return 0U;
            if (!append_text(buf, buf_size, &offset, ",\"name\":")) return 0U;
            if (!append_json_string(buf, buf_size, &offset, op->name)) return 0U;
            if (!append_text(buf, buf_size, &offset, ",\"type\":")) return 0U;
            if (!append_json_string(buf, buf_size, &offset, "free_output")) return 0U;
            if (!append_text(buf, buf_size, &offset, ",\"node_name\":")) return 0U;
            if (!append_json_string(buf, buf_size, &offset, "Gateway")) return 0U;
            if (!append_text(buf, buf_size, &offset, ",\"location\":")) return 0U;
            if (!append_json_string(buf, buf_size, &offset, "Gateway")) return 0U;
            if (!append_text(buf, buf_size, &offset, ",\"category\":")) return 0U;
            if (!append_json_string(buf, buf_size, &offset, "")) return 0U;
            if (!append_text(buf, buf_size, &offset, ",\"binding\":{")) return 0U;
            if (op->gpio != (gpio_num_t)-1) {
                if (!append_format(buf, buf_size, &offset, "\"type\":\"gpio\",\"gpio\":%d", op->gpio)) return 0U;
            } else {
                if (!append_text(buf, buf_size, &offset, "\"type\":\"none\"")) return 0U;
            }
            if (!append_text(buf, buf_size, &offset, "},\"state\":\"FREE\",\"health\":\"Healthy\"}")) return 0U;
        }
    }

    if (!append_text(buf, buf_size, &offset, "]}"))
        return 0U;

    return offset;
}

static esp_err_t send_resources_json(httpd_req_t *req)
{
    char *json = (char *)json_buffer_malloc(INSTALLATION_MAP_JSON_BUFFER_SIZE);
    size_t len = 0U;

    if (!json) {
        json_buffer_free(NULL);
        httpd_resp_set_status(req, "500 Internal Server Error");
        httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"no_memory\"}");
        return ESP_OK;
    }

    len = build_resources_json(json, INSTALLATION_MAP_JSON_BUFFER_SIZE);
    if (len == 0U)
    {
        json_buffer_free(json);
        httpd_resp_set_status(req, "500 Internal Server Error");
        httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"resources_build_failed\"}");
        return ESP_OK;
    }

    httpd_resp_send(req, json, len);
    json_buffer_free(json);
    return ESP_OK;
}

static const char *rs485_link_state_name(const rs485_engine_metrics_t *engine,
                                         const rs485_master_metrics_t *master,
                                         const rs485_hal_metrics_t *hal)
{
    if (!hal || !hal->initialized || !engine || !engine->enabled)
        return "disabled";

    if (engine->self_test_enabled)
        return engine->self_test_active ? "self-test-active" : "self-test-ready";

    if (master && master->online_nodes > 0U)
        return "online";

    if (engine->rx_count > 0U && master && master->ack_count == 0U)
        return "rx-without-session";

    if (engine->tx_count > 0U && engine->rx_count == 0U)
        return "silent";

    if (engine->tx_count == 0U && engine->rx_count == 0U)
        return "idle";

    return "probing";
}

static const char *rs485_comm_state_name(const rs485_engine_metrics_t *engine,
                                         const rs485_master_metrics_t *master)
{
    if (!engine || !engine->enabled)
        return "disabled";

    if (engine->self_test_enabled)
        return engine->self_test_active ? "self-test-active" : "self-test-enabled";

    if (master && master->online_nodes > 0U)
        return "cluster-active";

    if (engine->rx_count > 0U)
        return "frames-received";

    if (engine->tx_count > 0U)
        return "cluster-idle";

    return "idle";
}

static const io_driver_input_diag_t *find_input_diag_by_id(uint16_t id, int count)
{
    for (int i = 0; i < count; i++)
    {
        if (status_input_diag_snapshot[i].id == id)
            return &status_input_diag_snapshot[i];
    }

    return NULL;
}

static const char *wifi_authmode_name(uint8_t authmode)
{
    switch ((wifi_auth_mode_t)authmode)
    {
        case WIFI_AUTH_OPEN:
            return "open";
        case WIFI_AUTH_WEP:
            return "wep";
        case WIFI_AUTH_WPA_PSK:
            return "wpa-psk";
        case WIFI_AUTH_WPA2_PSK:
            return "wpa2-psk";
        case WIFI_AUTH_WPA_WPA2_PSK:
            return "wpa-wpa2-psk";
#ifdef WIFI_AUTH_WPA2_ENTERPRISE
        case WIFI_AUTH_WPA2_ENTERPRISE:
            return "wpa2-enterprise";
#endif
#ifdef WIFI_AUTH_WPA3_PSK
        case WIFI_AUTH_WPA3_PSK:
            return "wpa3-psk";
#endif
#ifdef WIFI_AUTH_WPA2_WPA3_PSK
        case WIFI_AUTH_WPA2_WPA3_PSK:
            return "wpa2-wpa3-psk";
#endif
#ifdef WIFI_AUTH_WAPI_PSK
        case WIFI_AUTH_WAPI_PSK:
            return "wapi-psk";
#endif
        default:
            return "unknown";
    }
}


static bool append_gpio_option_array(char *buf, size_t buf_size, size_t *offset, bool inputs)
{
    int count = inputs ? device_profile_input_gpio_option_count() : device_profile_output_gpio_option_count();
    bool first = true;

    if (!append_text(buf, buf_size, offset, "["))
        return false;

    for (int i = 0; i < count; i++)
    {
        const device_gpio_option_t *option = inputs
            ? device_profile_input_gpio_option_at(i)
            : device_profile_output_gpio_option_at(i);
        bool allowed;

        if (!option)
            continue;

        allowed = inputs
            ? device_profile_input_gpio_allowed(option->gpio)
            : device_profile_output_gpio_allowed(option->gpio);

        if (!allowed)
            continue;

        if (!append_text(buf, buf_size, offset, first ? "" : ","))
            return false;

        if (!append_format(buf, buf_size, offset, "{\"id\":%d,\"name\":", (int)option->gpio))
            return false;

        if (!append_json_string(buf, buf_size, offset, option->label ? option->label : ""))
            return false;

        if (!append_text(buf, buf_size, offset, "}"))
            return false;

        first = false;
    }

    return append_text(buf, buf_size, offset, "]");
}

static bool preview_gpio_reserved_for_platform(gpio_num_t gpio, bool rs485_enabled)
{
    if (!rs485_enabled)
        return false;

    return gpio == GPIO_NUM_25 ||
           gpio == GPIO_NUM_26 ||
           gpio == GPIO_NUM_27;
}

static bool preview_gpio_reserved_for_network(gpio_num_t gpio,
                                              const device_network_profile_t *network,
                                              bool ethernet_enabled)
{
    const device_network_w5500_profile_t *w5500;

    if (!ethernet_enabled || !network || !network->ethernet_supported)
        return false;

    if (network->ethernet_mode != DEVICE_PROFILE_ETH_SPI_W5500 ||
        !device_profile_w5500_is_configured())
    {
        return false;
    }

    w5500 = device_profile_w5500();
    if (!w5500)
        return false;

    return gpio == w5500->mosi_gpio ||
           gpio == w5500->miso_gpio ||
           gpio == w5500->sclk_gpio ||
           gpio == w5500->cs_gpio ||
           gpio == w5500->int_gpio ||
           gpio == w5500->reset_gpio;
}

static bool preview_gpio_option_allowed(gpio_num_t gpio,
                                        const device_network_profile_t *network,
                                        bool ethernet_enabled,
                                        bool rs485_enabled)
{
    return !preview_gpio_reserved_for_platform(gpio, rs485_enabled) &&
           !preview_gpio_reserved_for_network(gpio, network, ethernet_enabled);
}

static bool append_gpio_option_array_preview(char *buf,
                                             size_t buf_size,
                                             size_t *offset,
                                             bool inputs,
                                             const device_network_profile_t *network,
                                             bool ethernet_enabled,
                                             bool rs485_enabled)
{
    int count = inputs ? device_profile_input_gpio_option_count() : device_profile_output_gpio_option_count();
    bool first = true;

    if (!append_text(buf, buf_size, offset, "["))
        return false;

    for (int i = 0; i < count; i++)
    {
        const device_gpio_option_t *option = inputs
            ? device_profile_input_gpio_option_at(i)
            : device_profile_output_gpio_option_at(i);

        if (!option)
            continue;

        if (!preview_gpio_option_allowed(option->gpio, network, ethernet_enabled, rs485_enabled))
            continue;

        if (!append_text(buf, buf_size, offset, first ? "" : ","))
            return false;

        if (!append_format(buf, buf_size, offset, "{\"id\":%d,\"name\":", (int)option->gpio))
            return false;

        if (!append_json_string(buf, buf_size, offset, option->label ? option->label : ""))
            return false;

        if (!append_text(buf, buf_size, offset, "}"))
            return false;

        first = false;
    }

    return append_text(buf, buf_size, offset, "]");
}

static void preview_reserved_gpio_push(int *pins, int *count, gpio_num_t gpio)
{
    int value = (int)gpio;

    if (!pins || !count || value < 0)
        return;

    for (int i = 0; i < *count; i++)
    {
        if (pins[i] == value)
            return;
    }

    if (*count >= 9)
        return;

    pins[*count] = value;
    (*count)++;
}

static bool append_reserved_gpio_array_preview(char *buf,
                                               size_t buf_size,
                                               size_t *offset,
                                               const device_network_profile_t *network,
                                               bool ethernet_enabled,
                                               bool rs485_enabled)
{
    int pins[9];
    int count = 0;

    memset(pins, 0, sizeof(pins));

    if (rs485_enabled)
    {
        preview_reserved_gpio_push(pins, &count, GPIO_NUM_25);
        preview_reserved_gpio_push(pins, &count, GPIO_NUM_26);
        preview_reserved_gpio_push(pins, &count, GPIO_NUM_27);
    }

    if (ethernet_enabled &&
        network &&
        network->ethernet_supported &&
        network->ethernet_mode == DEVICE_PROFILE_ETH_SPI_W5500 &&
        device_profile_w5500_is_configured())
    {
        const device_network_w5500_profile_t *w5500 = device_profile_w5500();

        if (w5500)
        {
            preview_reserved_gpio_push(pins, &count, w5500->mosi_gpio);
            preview_reserved_gpio_push(pins, &count, w5500->miso_gpio);
            preview_reserved_gpio_push(pins, &count, w5500->sclk_gpio);
            preview_reserved_gpio_push(pins, &count, w5500->cs_gpio);
            preview_reserved_gpio_push(pins, &count, w5500->int_gpio);
            preview_reserved_gpio_push(pins, &count, w5500->reset_gpio);
        }
    }

    if (!append_text(buf, buf_size, offset, "["))
        return false;

    for (int i = 0; i < count; i++)
    {
        if (!append_text(buf, buf_size, offset, (i == 0) ? "" : ","))
            return false;

        if (!append_format(buf, buf_size, offset, "%d", pins[i]))
            return false;
    }

    return append_text(buf, buf_size, offset, "]");
}

static void installation_local_code(bool input, int local_index, char out_code[INSTALLATION_MAP_LOCAL_CODE_LEN])
{
    snprintf(out_code, INSTALLATION_MAP_LOCAL_CODE_LEN, "%s%u", input ? "IN" : "OUT", (unsigned)(local_index & 0xFFFF));
}

static const plc_channel_desc_t *find_plc_channel_desc(device_channel_class_t cls, int index)
{
    const node_profile_desc_t *current_tpl = device_profile_get_current();
    if (!current_tpl || !current_tpl->plc_channels)
        return NULL;

    for (size_t p = 0; p < current_tpl->plc_channels_len; p++)
    {
        if (current_tpl->plc_channels[p].channel_class == cls &&
            current_tpl->plc_channels[p].channel_index == (uint16_t)index)
        {
            return &current_tpl->plc_channels[p];
        }
    }
    return NULL;
}

static bool append_available_slot_array(char *buf, size_t buf_size, size_t *offset, bool inputs)
{
    uint16_t ids[(IO_BINDING_MAX_INPUTS > IO_BINDING_MAX_OUTPUTS) ? IO_BINDING_MAX_INPUTS : IO_BINDING_MAX_OUTPUTS];
    int count = inputs
        ? io_binding_available_inputs(ids, (int)(sizeof(ids) / sizeof(ids[0])))
        : io_binding_available_outputs(ids, (int)(sizeof(ids) / sizeof(ids[0])));

    if (!append_text(buf, buf_size, offset, "["))
        return false;

    for (int i = 0; i < count; i++)
    {
        if (i > 0 && !append_text(buf, buf_size, offset, ","))
            return false;

        if (!append_format(buf, buf_size, offset, "{\"id\":%u,\"name\":", ids[i]))
            return false;

        if (inputs)
        {
            const device_input_profile_t *profile = device_profile_find_input(ids[i]);
            if (!append_json_string(buf, buf_size, offset, profile ? profile->name : "Entrada"))
                return false;
        }
        else
        {
            const device_output_profile_t *profile = device_profile_find_output(ids[i]);
            if (!append_json_string(buf, buf_size, offset, profile ? profile->name : "Saída"))
                return false;
        }

        if (!append_text(buf, buf_size, offset, "}"))
            return false;
    }

    return append_text(buf, buf_size, offset, "]");
}

static bool profile_backend_supported(device_channel_backend_t backend)
{
    const device_node_capabilities_t *node_caps = device_profile_node_capabilities();
    const device_expansion_capabilities_t *expansion = device_profile_expansion_capabilities();

    switch (backend)
    {
        case DEVICE_CHANNEL_BACKEND_GPIO:
            return true;
        case DEVICE_CHANNEL_BACKEND_MCP23X17:
            return expansion && expansion->supports_mcp23x17;
        case DEVICE_CHANNEL_BACKEND_ADC_NATIVE:
            return (node_caps && node_caps->supports_native_analog) ||
                   (expansion && expansion->native_analog_input_channels > 0U);
        case DEVICE_CHANNEL_BACKEND_ADC_EXTERNAL:
            return (node_caps && node_caps->supports_external_analog) ||
                   (expansion && expansion->supports_ads1115);
        default:
            return false;
    }
}

static void profile_backend_active_counts(device_channel_backend_t backend,
                                          int input_count,
                                          int output_count,
                                          int *active_inputs,
                                          int *active_outputs)
{
    int inputs = 0;
    int outputs = 0;

    for (int i = 0; i < input_count; i++)
    {
        if ((device_channel_backend_t)input_profile_snapshot[i].backend == backend)
            inputs++;
    }

    for (int i = 0; i < output_count; i++)
    {
        if ((device_channel_backend_t)output_profile_snapshot[i].backend == backend)
            outputs++;
    }

    if (active_inputs)
        *active_inputs = inputs;
    if (active_outputs)
        *active_outputs = outputs;
}

static const char *profile_component_transport_label(device_profile_transport_t transport)
{
    switch (transport)
    {
        case DEVICE_PROFILE_TRANSPORT_WIFI:
            return "Wi-Fi";
        case DEVICE_PROFILE_TRANSPORT_ETHERNET:
            return "Ethernet / RJ45";
        case DEVICE_PROFILE_TRANSPORT_RS485:
            return "RS485";
        case DEVICE_PROFILE_TRANSPORT_NONE:
        default:
            return "Transporte";
    }
}

static const char *profile_component_state_transport(const device_network_profile_t *network,
                                                     device_profile_transport_t transport)
{
    if (!device_profile_transport_supported(transport))
        return "unsupported";

    if (device_profile_transport_enabled(transport))
        return "enabled";

    (void)network;
    return "disabled";
}

static const char *profile_component_reason_transport(const device_network_profile_t *network,
                                                      device_profile_transport_t transport)
{
    if (!device_profile_transport_supported(transport))
        return "O hardware/perfil atual nao suporta este transporte.";

    if (!device_profile_transport_enabled(transport))
    {
        if (transport == DEVICE_PROFILE_TRANSPORT_ETHERNET &&
            network &&
            network->ethernet_supported &&
            !device_profile_w5500_is_configured())
        {
            return "O transporte esta previsto no perfil, mas a camada fisica ainda nao foi configurada.";
        }

        return "O transporte esta desabilitado na configuracao ativa deste no.";
    }

    return "O transporte participa do runtime ativo deste no.";
}

static const char *profile_component_action_transport(const device_network_profile_t *network,
                                                      device_profile_transport_t transport)
{
    if (!device_profile_transport_supported(transport))
        return "Use hardware ou perfil compativel para habilitar esta capacidade.";

    if (!device_profile_transport_enabled(transport))
    {
        if (transport == DEVICE_PROFILE_TRANSPORT_ETHERNET &&
            network &&
            network->ethernet_supported &&
            !device_profile_w5500_is_configured())
        {
            return "Configure os GPIOs e o modulo W5500 antes de ativar este enlace.";
        }

        return "Ative este transporte no perfil do no somente quando ele for realmente necessario.";
    }

    return "Nenhuma acao imediata e necessaria.";
}

static const char *profile_component_state_backend(const io_binding_backend_view_t *backend_view,
                                                   bool supported,
                                                   int active_inputs,
                                                   int active_outputs)
{
    if (!supported)
        return "unsupported";

    if ((active_inputs + active_outputs) > 0)
        return "enabled";

    if (backend_view && (backend_view->selectable_now || backend_view->implemented_now))
        return "disabled";

    return "hidden";
}

static const char *profile_component_reason_backend(const io_binding_backend_view_t *backend_view,
                                                    bool supported,
                                                    int active_inputs,
                                                    int active_outputs)
{
    if (!supported)
        return "O hardware/perfil atual nao suporta este backend neste no.";

    if ((active_inputs + active_outputs) > 0)
        return "Existe canal ativo usando este backend na configuracao atual.";

    if (backend_view && (backend_view->selectable_now || backend_view->implemented_now))
        return "O backend esta disponivel, mas sem canal ativo na configuracao atual.";

    return "O backend segue no catalogo tecnico, mas ainda fora da superficie principal de operacao.";
}

static const char *profile_component_action_backend(const io_binding_backend_view_t *backend_view,
                                                    bool supported,
                                                    int active_inputs,
                                                    int active_outputs)
{
    if (!supported)
        return "Use um perfil ou expansao compativel para habilitar este backend.";

    if ((active_inputs + active_outputs) > 0)
        return "Nenhuma acao imediata e necessaria.";

    if (backend_view && backend_view->selectable_now)
        return "Vincule um canal a este backend quando a expansao local fizer sentido.";

    return "Mantenha este item apenas como referencia tecnica ate a fase correspondente do roadmap.";
}

static bool append_node_capabilities_object(char *buf, size_t buf_size, size_t *offset)
{
    const device_node_capabilities_t *caps = device_profile_node_capabilities();

    if (!caps)
        return append_text(buf, buf_size, offset, "{}");

    if (!append_format(buf,
                       buf_size,
                       offset,
                       "{\"local_input_slot_capacity\":%u,\"local_output_slot_capacity\":%u,"
                       "\"default_input_count\":%u,\"default_output_count\":%u,"
                       "\"distributed_scaling\":%u,\"supports_remote_nodes\":%u,"
                       "\"supports_mcp_digital\":%u,\"supports_native_analog\":%u,"
                       "\"supports_external_analog\":%u,\"global_capacity_mode\":",
                       caps->local_input_slot_capacity,
                       caps->local_output_slot_capacity,
                       caps->default_input_count,
                       caps->default_output_count,
                       caps->distributed_scaling ? 1U : 0U,
                       caps->supports_remote_nodes ? 1U : 0U,
                       caps->supports_mcp_digital ? 1U : 0U,
                       caps->supports_native_analog ? 1U : 0U,
                       caps->supports_external_analog ? 1U : 0U))
    {
        return false;
    }

    if (!append_json_string(buf, buf_size, offset, caps->global_capacity_mode))
        return false;

    if (!append_text(buf, buf_size, offset, ",\"local_capacity_mode\":"))
        return false;

    if (!append_json_string(buf, buf_size, offset, caps->local_capacity_mode))
        return false;

    if (!append_text(buf, buf_size, offset, ",\"recommended_scaling_path\":"))
        return false;

    if (!append_json_string(buf, buf_size, offset, caps->recommended_scaling_path))
        return false;

    return append_text(buf, buf_size, offset, "}");
}

static bool append_expansion_capabilities_object(char *buf, size_t buf_size, size_t *offset)
{
    const device_expansion_capabilities_t *expansion = device_profile_expansion_capabilities();

    if (!expansion)
        return append_text(buf, buf_size, offset, "{}");

    if (!append_format(buf,
                       buf_size,
                       offset,
                       "{\"supports_mcp23x17\":%u,\"recommended_mcp_instances\":%u,"
                       "\"channels_per_mcp\":%u,\"supports_ads1115\":%u,"
                       "\"recommended_external_adc_instances\":%u,"
                       "\"channels_per_external_adc\":%u,\"native_analog_input_channels\":%u,"
                       "\"notes\":",
                       expansion->supports_mcp23x17 ? 1U : 0U,
                       expansion->recommended_mcp_instances,
                       expansion->channels_per_mcp,
                       expansion->supports_ads1115 ? 1U : 0U,
                       expansion->recommended_external_adc_instances,
                       expansion->channels_per_external_adc,
                       expansion->native_analog_input_channels))
    {
        return false;
    }

    if (!append_json_string(buf, buf_size, offset, expansion->notes))
        return false;

    return append_text(buf, buf_size, offset, "}");
}

static bool append_channel_inventory_array(char *buf, size_t buf_size, size_t *offset)
{
    int count = device_profile_channel_inventory_group_count();

    if (!append_text(buf, buf_size, offset, "["))
        return false;

    for (int i = 0; i < count; i++)
    {
        const device_channel_inventory_group_t *group = device_profile_channel_inventory_group_at(i);

        if (!group)
            continue;

        if (!append_text(buf, buf_size, offset, (i == 0) ? "" : ","))
            return false;

        if (!append_text(buf, buf_size, offset, "{\"group_id\":"))
            return false;

        if (!append_json_string(buf, buf_size, offset, group->group_id))
            return false;

        if (!append_text(buf, buf_size, offset, ",\"label\":"))
            return false;

        if (!append_json_string(buf, buf_size, offset, group->label))
            return false;

        if (!append_text(buf, buf_size, offset, ",\"backend\":"))
            return false;

        if (!append_json_string(buf, buf_size, offset, io_binding_backend_code(group->backend)))
            return false;

        if (!append_format(buf,
                           buf_size,
                           offset,
                           ",\"nominal_capacity\":%u,\"default_slots\":%u,"
                           "\"implemented_now\":%u,\"dashboard_ready\":%u,"
                           "\"expansion_path\":%u,\"addressing_model\":",
                           group->nominal_capacity,
                           group->default_slots,
                           group->implemented_now ? 1U : 0U,
                           group->dashboard_ready ? 1U : 0U,
                           group->expansion_path ? 1U : 0U))
        {
            return false;
        }

        if (!append_json_string(buf, buf_size, offset, group->addressing_model))
            return false;

        if (!append_text(buf, buf_size, offset, ",\"notes\":"))
            return false;

        if (!append_json_string(buf, buf_size, offset, group->notes))
            return false;

        if (!append_text(buf, buf_size, offset, "}"))
            return false;
    }

    return append_text(buf, buf_size, offset, "]");
}

static bool append_binding_backends_array(char *buf, size_t buf_size, size_t *offset)
{
    int count = io_binding_backend_count();

    if (!append_text(buf, buf_size, offset, "["))
        return false;

    for (int i = 0; i < count; i++)
    {
        const io_binding_backend_view_t *backend = io_binding_backend_at(i);

        if (!backend)
            continue;

        if (!append_text(buf, buf_size, offset, (i == 0) ? "" : ","))
            return false;

        if (!append_text(buf, buf_size, offset, "{\"code\":"))
            return false;

        if (!append_json_string(buf, buf_size, offset, backend->backend_code))
            return false;

        if (!append_text(buf, buf_size, offset, ",\"label\":"))
            return false;

        if (!append_json_string(buf, buf_size, offset, backend->label))
            return false;

        if (!append_format(buf,
                           buf_size,
                           offset,
                           ",\"implemented_now\":%u,\"selectable_now\":%u,\"expansion_path\":%u,\"notes\":",
                           backend->implemented_now ? 1U : 0U,
                           backend->selectable_now ? 1U : 0U,
                           backend->expansion_path ? 1U : 0U))
        {
            return false;
        }

        if (!append_json_string(buf, buf_size, offset, backend->notes))
            return false;

        if (!append_text(buf, buf_size, offset, "}"))
            return false;
    }

    return append_text(buf, buf_size, offset, "]");
}

static bool append_capability_resolution_object(char *buf,
                                                size_t buf_size,
                                                size_t *offset,
                                                const device_network_profile_t *network,
                                                int input_count,
                                                int output_count)
{
    if (!append_text(buf, buf_size, offset,
                     "{\"model\":\"hardware-profile-active-config\","
                     "\"precedence\":[\"hardware\",\"profile\",\"active_config\"],"
                     "\"hardware\":{"))
    {
        return false;
    }

    if (!append_format(buf,
                       buf_size,
                       offset,
                       "\"wifi_supported\":%u,\"ethernet_supported\":%u,\"rs485_supported\":%u",
                       (network && network->wifi_supported) ? 1U : 0U,
                       (network && network->ethernet_supported) ? 1U : 0U,
                       (network && network->rs485_supported) ? 1U : 0U))
    {
        return false;
    }

    if (!append_format(buf,
                       buf_size,
                       offset,
                       "},\"profile\":{\"input_slot_capacity\":%d,\"output_slot_capacity\":%d},"
                       "\"active_config\":{\"active_input_count\":%d,\"active_output_count\":%d,"
                       "\"wifi_enabled\":%u,\"ethernet_enabled\":%u,\"rs485_enabled\":%u,\"wifi_mode\":%d},"
                       "\"effective_rule\":",
                       device_profile_input_count(),
                       device_profile_output_count(),
                       input_count,
                       output_count,
                       (network && network->wifi_enabled) ? 1U : 0U,
                       (network && network->ethernet_enabled) ? 1U : 0U,
                       (network && network->rs485_enabled) ? 1U : 0U,
                       (network) ? (int)network->wifi_mode : 0))
    {
        return false;
    }

    if (!append_json_string(buf,
                            buf_size,
                            offset,
                            "A capacidade efetiva combina hardware suportado, perfil do no e configuracao ativa persistida."))
    {
        return false;
    }

    return append_text(buf, buf_size, offset, "}");
}

static bool append_component_catalog_array(char *buf,
                                           size_t buf_size,
                                           size_t *offset,
                                           const device_network_profile_t *network,
                                           int input_count,
                                           int output_count)
{
    bool first = true;

    if (!append_text(buf, buf_size, offset, "["))
        return false;

    for (int i = 0; i < 3; i++)
    {
        device_profile_transport_t transport = (device_profile_transport_t)(i + 1);
        const char *state = profile_component_state_transport(network, transport);
        bool supported = device_profile_transport_supported(transport);
        bool active_now = device_profile_transport_enabled(transport);

        if (!append_text(buf, buf_size, offset, first ? "" : ","))
            return false;

        if (!append_format(buf,
                           buf_size,
                           offset,
                           "{\"id\":\"transport:%s\",\"category\":\"transport\",\"label\":",
                           (transport == DEVICE_PROFILE_TRANSPORT_WIFI) ? "wifi" :
                           (transport == DEVICE_PROFILE_TRANSPORT_ETHERNET) ? "ethernet" :
                                                                           "rs485"))
        {
            return false;
        }

        if (!append_json_string(buf, buf_size, offset, profile_component_transport_label(transport)))
            return false;

        if (!append_text(buf, buf_size, offset, ",\"state\":"))
            return false;

        if (!append_json_string(buf, buf_size, offset, state))
            return false;

        if (!append_format(buf,
                           buf_size,
                           offset,
                           ",\"supported\":%u,\"active_now\":%u,\"visible_in_main\":%u,"
                           "\"implemented_now\":1,\"configurable_now\":%u,\"reason\":",
                           supported ? 1U : 0U,
                           active_now ? 1U : 0U,
                           active_now ? 1U : 0U,
                           supported ? 1U : 0U))
        {
            return false;
        }

        if (!append_json_string(buf,
                                buf_size,
                                offset,
                                profile_component_reason_transport(network, transport)))
        {
            return false;
        }

        if (!append_text(buf, buf_size, offset, ",\"recommended_action\":"))
            return false;

        if (!append_json_string(buf,
                                buf_size,
                                offset,
                                profile_component_action_transport(network, transport)))
        {
            return false;
        }

        if (!append_text(buf, buf_size, offset, "}"))
            return false;

        first = false;
    }

    for (int i = 0; i < io_binding_backend_count(); i++)
    {
        const io_binding_backend_view_t *backend = io_binding_backend_at(i);
        int active_inputs = 0;
        int active_outputs = 0;
        bool supported;
        const char *state;

        if (!backend)
            continue;

        supported = profile_backend_supported(backend->backend);
        profile_backend_active_counts(backend->backend,
                                      input_count,
                                      output_count,
                                      &active_inputs,
                                      &active_outputs);
        state = profile_component_state_backend(backend,
                                                supported,
                                                active_inputs,
                                                active_outputs);

        if (!append_text(buf, buf_size, offset, first ? "" : ","))
            return false;

        if (!append_text(buf, buf_size, offset, "{\"id\":\"backend:"))
            return false;

        if (!append_text(buf, buf_size, offset, backend->backend_code))
            return false;

        if (!append_text(buf, buf_size, offset, "\",\"category\":\"channel-backend\",\"label\":"))
            return false;

        if (!append_json_string(buf, buf_size, offset, backend->label))
            return false;

        if (!append_text(buf, buf_size, offset, ",\"state\":"))
            return false;

        if (!append_json_string(buf, buf_size, offset, state))
            return false;

        if (!append_format(buf,
                           buf_size,
                           offset,
                           ",\"supported\":%u,\"active_now\":%u,\"visible_in_main\":%u,"
                           "\"implemented_now\":%u,\"configurable_now\":%u,"
                           "\"active_inputs\":%d,\"active_outputs\":%d,\"reason\":",
                           supported ? 1U : 0U,
                           (active_inputs + active_outputs) > 0 ? 1U : 0U,
                           (active_inputs + active_outputs) > 0 ? 1U : 0U,
                           backend->implemented_now ? 1U : 0U,
                           backend->selectable_now ? 1U : 0U,
                           active_inputs,
                           active_outputs))
        {
            return false;
        }

        if (!append_json_string(buf,
                                buf_size,
                                offset,
                                profile_component_reason_backend(backend,
                                                                 supported,
                                                                 active_inputs,
                                                                 active_outputs)))
        {
            return false;
        }

        if (!append_text(buf, buf_size, offset, ",\"recommended_action\":"))
            return false;

        if (!append_json_string(buf,
                                buf_size,
                                offset,
                                profile_component_action_backend(backend,
                                                                 supported,
                                                                 active_inputs,
                                                                 active_outputs)))
        {
            return false;
        }

        if (!append_text(buf, buf_size, offset, "}"))
            return false;

        first = false;
    }

    return append_text(buf, buf_size, offset, "]");
}

static bool append_mcp_runtime_object(char *buf, size_t buf_size, size_t *offset)
{
    io_binding_mcp_instance_view_t instances[4];
    int total = io_binding_export_mcp_instances(instances, (int)(sizeof(instances) / sizeof(instances[0])));

    if (!append_text(buf, buf_size, offset, "{\"instances\":["))
        return false;

    for (int i = 0; i < total && i < (int)(sizeof(instances) / sizeof(instances[0])); i++)
    {
        if (!append_text(buf, buf_size, offset, (i == 0) ? "" : ","))
            return false;

        if (!append_format(buf,
                           buf_size,
                           offset,
                           "{\"instance\":%d,\"label\":",
                           instances[i].instance))
        {
            return false;
        }

        if (!append_json_string(buf, buf_size, offset, instances[i].label))
            return false;

        if (!append_format(buf,
                           buf_size,
                           offset,
                           ",\"channel_capacity\":%d,\"active_inputs\":%d,\"active_outputs\":%d,"
                           "\"active_total\":%d,\"configurable_now\":%u,\"runtime_contract_ready\":%u,"
                           "\"hardware_runtime_ready\":%u,\"notes\":",
                           instances[i].channel_capacity,
                           instances[i].active_inputs,
                           instances[i].active_outputs,
                           instances[i].active_total,
                           instances[i].configurable_now ? 1U : 0U,
                           instances[i].runtime_contract_ready ? 1U : 0U,
                           instances[i].hardware_runtime_ready ? 1U : 0U))
        {
            return false;
        }

        if (!append_json_string(buf, buf_size, offset, instances[i].notes))
            return false;

        if (!append_text(buf, buf_size, offset, "}"))
            return false;
    }

    if (!append_text(buf, buf_size, offset, "],\"endpoints\":[]}"))
        return false;

    return true;
}

static bool append_profile_context_sections(char *buf,
                                            size_t buf_size,
                                            size_t *offset,
                                            const device_network_profile_t *network,
                                            int input_count,
                                            int output_count)
{
    if (!append_text(buf, buf_size, offset, ",\"node_capabilities\":"))
        return false;

    if (!append_node_capabilities_object(buf, buf_size, offset))
        return false;

    if (!append_text(buf, buf_size, offset, ",\"capability_resolution\":"))
        return false;

    if (!append_capability_resolution_object(buf,
                                             buf_size,
                                             offset,
                                             network,
                                             input_count,
                                             output_count))
    {
        return false;
    }

    if (!append_text(buf, buf_size, offset, ",\"expansion_capabilities\":"))
        return false;

    if (!append_expansion_capabilities_object(buf, buf_size, offset))
        return false;

    if (!append_text(buf, buf_size, offset, ",\"channel_inventory\":"))
        return false;

    if (!append_channel_inventory_array(buf, buf_size, offset))
        return false;

    if (!append_text(buf, buf_size, offset, ",\"binding_backends\":"))
        return false;

    if (!append_binding_backends_array(buf, buf_size, offset))
        return false;

    if (!append_text(buf, buf_size, offset, ",\"components\":"))
        return false;

    if (!append_component_catalog_array(buf,
                                        buf_size,
                                        offset,
                                        network,
                                        input_count,
                                        output_count))
    {
        return false;
    }

    if (!append_text(buf, buf_size, offset, ",\"mcp_runtime\":"))
        return false;

    if (!append_mcp_runtime_object(buf, buf_size, offset))
        return false;

    return true;
}

static size_t build_network_preview_json(char *buf,
                                         size_t buf_size,
                                         bool wifi_enabled,
                                         bool ethernet_enabled,
                                         bool rs485_enabled)
{
    const device_network_profile_t *network = device_profile_network();
    size_t offset = 0;
    bool preview_wifi;
    bool preview_ethernet;
    bool preview_rs485;

    if (!buf || buf_size == 0U)
        return 0;

    buf[0] = '\0';

    preview_wifi = network && network->wifi_supported && wifi_enabled;
    preview_ethernet = network && network->ethernet_supported && ethernet_enabled;
    preview_rs485 = network && network->rs485_supported && rs485_enabled;

    if (!append_format(buf,
                       buf_size,
                       &offset,
                       "{\"wifi_enabled\":%u,\"ethernet_enabled\":%u,\"rs485_enabled\":%u,"
                       "\"input_gpio_options\":",
                       preview_wifi ? 1U : 0U,
                       preview_ethernet ? 1U : 0U,
                       preview_rs485 ? 1U : 0U))
    {
        return 0;
    }

    if (!append_gpio_option_array_preview(buf,
                                          buf_size,
                                          &offset,
                                          true,
                                          network,
                                          preview_ethernet,
                                          preview_rs485))
    {
        return 0;
    }

    if (!append_text(buf, buf_size, &offset, ",\"output_gpio_options\":"))
        return 0;

    if (!append_gpio_option_array_preview(buf,
                                          buf_size,
                                          &offset,
                                          false,
                                          network,
                                          preview_ethernet,
                                          preview_rs485))
    {
        return 0;
    }

    if (!append_text(buf, buf_size, &offset, ",\"reserved_gpios\":"))
        return 0;

    if (!append_reserved_gpio_array_preview(buf,
                                            buf_size,
                                            &offset,
                                            network,
                                            preview_ethernet,
                                            preview_rs485))
    {
        return 0;
    }

    if (!append_text(buf, buf_size, &offset, "}"))
        return 0;

    return offset;
}

static bool kernel_load_phase_from_text(const char *text, uint8_t *phase)
{
    if (!text || !phase)
        return false;

    if (strcmp(text, "io") == 0)
        *phase = PHASE_LOAD_TEST_IO;
    else if (strcmp(text, "io_apply") == 0)
        *phase = PHASE_LOAD_TEST_IO_APPLY;
    else if (strcmp(text, "fieldbus") == 0)
        *phase = PHASE_LOAD_TEST_FIELDBUS;
    else if (strcmp(text, "automation") == 0)
        *phase = PHASE_LOAD_TEST_AUTOMATION;
    else if (strcmp(text, "events") == 0)
        *phase = PHASE_LOAD_TEST_EVENTS;
    else
        return false;

    return true;
}

static bool cluster_transport_visible(cluster_transport_type_t type,
                                      const device_network_profile_t *network)
{
    switch (type)
    {
        case CLUSTER_TRANSPORT_WIFI_UDP:
            return network_transport_wifi_enabled(network);
        case CLUSTER_TRANSPORT_ETHERNET_UDP:
            return network_transport_ethernet_enabled(network);
        case CLUSTER_TRANSPORT_RS485:
            return network_transport_rs485_enabled(network);
        case CLUSTER_TRANSPORT_NONE:
        default:
            return true;
    }
}

static uint8_t node_registry_transport_sanitize(uint8_t transport,
                                                const device_network_profile_t *network)
{
    switch ((node_registry_transport_t)transport)
    {
        case NODE_REGISTRY_TRANSPORT_WIFI_UDP:
            return network_transport_wifi_enabled(network)
                ? transport
                : NODE_REGISTRY_TRANSPORT_NONE;
        case NODE_REGISTRY_TRANSPORT_ETHERNET_UDP:
            return network_transport_ethernet_enabled(network)
                ? transport
                : NODE_REGISTRY_TRANSPORT_NONE;
        case NODE_REGISTRY_TRANSPORT_RS485_CLUSTER:
            return network_transport_rs485_enabled(network)
                ? transport
                : NODE_REGISTRY_TRANSPORT_NONE;
        case NODE_REGISTRY_TRANSPORT_NONE:
        default:
            return NODE_REGISTRY_TRANSPORT_NONE;
    }
}

static uint8_t node_registry_offline_reason_sanitize(uint8_t reason,
                                                     uint8_t sanitized_transport)
{
    if (sanitized_transport == NODE_REGISTRY_TRANSPORT_NONE)
        return NODE_REGISTRY_OFFLINE_NONE;

    return reason;
}

static uint8_t node_registry_recovery_caps_sanitize(uint8_t caps,
                                                    const device_network_profile_t *network)
{
    uint8_t sanitized = caps;

    if (!network_transport_wifi_enabled(network))
    {
        sanitized &= (uint8_t)~(NODE_REGISTRY_RECOVERY_TRY_RECONNECT |
                                NODE_REGISTRY_RECOVERY_REENABLE_WIFI |
                                NODE_REGISTRY_RECOVERY_FORCE_MODE);
    }

    return sanitized;
}

static const char *node_profile_label_from_registry(const char *profile)
{
    if (!profile || profile[0] == '\0')
        return "Perfil pendente";

    if (strcmp(profile, "gateway") == 0)
        return "Gateway";
    if (strcmp(profile, "field-node") == 0)
        return "Field Node";
    if (strcmp(profile, "relay-node") == 0)
        return "Relay Node";
    if (strcmp(profile, "sensor-node") == 0)
        return "Sensor Node";
    if (strcmp(profile, "local-io-node") == 0)
        return "Local I/O Node";

    return "Perfil registrado";
}

static const char *node_context_state_from_registry(const node_registry_entry_t *node)
{
    if (!node || node->profile[0] == '\0')
        return "pending_profile";

    switch ((node_registry_state_t)node->registry_state)
    {
        case NODE_REGISTRY_STATE_ACTIVE:
            return "operational_context";
        case NODE_REGISTRY_STATE_CONFIGURED:
            return "configured_context";
        case NODE_REGISTRY_STATE_ADOPTED:
            return "adopted_context";
        case NODE_REGISTRY_STATE_DISCOVERED:
        default:
            return "pending_context";
    }
}

static const char *node_admission_phase_from_registry(const node_registry_entry_t *node)
{
    if (!node)
        return "pending";

    switch ((node_registry_state_t)node->registry_state)
    {
        case NODE_REGISTRY_STATE_ACTIVE:
            return "active";
        case NODE_REGISTRY_STATE_CONFIGURED:
            return "configured";
        case NODE_REGISTRY_STATE_ADOPTED:
            return "approved";
        case NODE_REGISTRY_STATE_DISCOVERED:
        default:
            return "pending";
    }
}

static const char *node_admission_action_from_registry(const node_registry_entry_t *node)
{
    if (!node)
        return "approve";

    switch ((node_registry_state_t)node->registry_state)
    {
        case NODE_REGISTRY_STATE_ACTIVE:
            return "revoke";
        case NODE_REGISTRY_STATE_CONFIGURED:
            return "activate";
        case NODE_REGISTRY_STATE_ADOPTED:
            return "configure";
        case NODE_REGISTRY_STATE_DISCOVERED:
        default:
            return "approve";
    }
}

static size_t build_nodes_json(char *buf, size_t buf_size)
{
    node_registry_entry_t entries[NODE_REGISTRY_MAX_NODES];
    const device_network_profile_t *network = device_profile_network();
    size_t offset = 0U;
    int count = 0;

    if (!buf || buf_size == 0U)
        return 0U;

    buf[0] = '\0';
    count = node_registry_export(entries, NODE_REGISTRY_MAX_NODES);

    if (!append_text(buf, buf_size, &offset, "{\"nodes\":["))
        return 0U;

    for (int i = 0; i < count; i++)
    {
        node_registry_entry_t node = entries[i];
        uint8_t sanitized_transport = node_registry_transport_sanitize(node.last_transport, network);
        uint8_t offline_reason = node_registry_offline_reason_sanitize(node.offline_reason, sanitized_transport);
        uint8_t recovery_caps = node_registry_recovery_caps_sanitize(node.recovery_capabilities, network);
        bool operational = node.registry_state == NODE_REGISTRY_STATE_ACTIVE;
        bool approval_required = node.registry_state == NODE_REGISTRY_STATE_DISCOVERED;
        const char *registry_state = node_registry_state_name((node_registry_state_t)node.registry_state);
        const char *cluster_state = node_registry_cluster_state_name(node.cluster_state);
        const char *transport = node_registry_transport_name(sanitized_transport);
        const char *offline = node_registry_offline_reason_name(offline_reason);
        const char *operational_state = operational ? "online" : (approval_required ? "pending" : "configured");
        const char *issue = operational ? "Nenhum problema operacional relevante" : "No aguardando conclusao de admissao";
        const char *action = operational ? "Nenhuma acao imediata e necessaria." : "Aprove, configure perfil/template e ative o no antes de operar.";
        ip4_addr_t ip = {0};
        char ip_text[20] = {0};

        ip.addr = node.last_ip_addr;
        if (node.last_ip_addr != 0U)
            ip4addr_ntoa_r(&ip, ip_text, sizeof(ip_text));

        if (sanitized_transport == NODE_REGISTRY_TRANSPORT_NONE)
            node.last_ip_addr = 0U;

        if (i > 0 && !append_text(buf, buf_size, &offset, ","))
            return 0U;

        if (!append_format(buf,
                           buf_size,
                           &offset,
                           "{\"id\":%" PRIu32 ",\"age_ms\":%" PRIu32 ",\"last_seen_ms\":%" PRIu32
                           ",\"health\":%u,\"operational\":%u,\"approval_required\":%u,"
                           "\"recovery_capabilities\":%u,\"registry_state\":",
                           node.node_id,
                           node.age_ms,
                           node.last_seen_ms,
                           node.health,
                           operational ? 1U : 0U,
                           approval_required ? 1U : 0U,
                           recovery_caps))
            return 0U;

        if (!append_json_string(buf, buf_size, &offset, registry_state))
            return 0U;
        if (!append_text(buf, buf_size, &offset, ",\"cluster_state\":"))
            return 0U;
        if (!append_json_string(buf, buf_size, &offset, cluster_state))
            return 0U;
        if (!append_text(buf, buf_size, &offset, ",\"profile\":"))
            return 0U;
        if (!append_json_string(buf, buf_size, &offset, node.profile))
            return 0U;
        if (!append_text(buf, buf_size, &offset, ",\"profile_label\":"))
            return 0U;
        if (!append_json_string(buf, buf_size, &offset, node_profile_label_from_registry(node.profile)))
            return 0U;
        if (!append_text(buf, buf_size, &offset, ",\"template\":"))
            return 0U;
        if (!append_json_string(buf, buf_size, &offset, node.template_name))
            return 0U;
        if (!append_text(buf, buf_size, &offset, ",\"ip\":"))
            return 0U;
        if (!append_json_string(buf, buf_size, &offset, ip_text))
            return 0U;
        if (!append_text(buf, buf_size, &offset, ",\"last_transport\":"))
            return 0U;
        if (!append_json_string(buf, buf_size, &offset, transport))
            return 0U;
        if (!append_text(buf, buf_size, &offset, ",\"offline_reason\":"))
            return 0U;
        if (!append_json_string(buf, buf_size, &offset, offline))
            return 0U;
        if (!append_text(buf, buf_size, &offset, ",\"operational_state\":"))
            return 0U;
        if (!append_json_string(buf, buf_size, &offset, operational_state))
            return 0U;
        if (!append_text(buf, buf_size, &offset, ",\"main_issue\":"))
            return 0U;
        if (!append_json_string(buf, buf_size, &offset, issue))
            return 0U;
        if (!append_text(buf, buf_size, &offset, ",\"recommended_action\":"))
            return 0U;
        if (!append_json_string(buf, buf_size, &offset, action))
            return 0U;
        if (!append_text(buf, buf_size, &offset, ",\"context_state\":"))
            return 0U;
        if (!append_json_string(buf, buf_size, &offset, node_context_state_from_registry(&node)))
            return 0U;
        if (!append_text(buf, buf_size, &offset, ",\"admission_phase\":"))
            return 0U;
        if (!append_json_string(buf, buf_size, &offset, node_admission_phase_from_registry(&node)))
            return 0U;
        if (!append_text(buf, buf_size, &offset, ",\"admission_action\":"))
            return 0U;
        if (!append_json_string(buf, buf_size, &offset, node_admission_action_from_registry(&node)))
            return 0U;
        if (!append_text(buf, buf_size, &offset, "}"))
            return 0U;
    }

    if (!append_text(buf, buf_size, &offset, "]}"))
        return 0U;

    return offset;
}

static void network_status_sanitize_snapshot(network_ready_snapshot_t *snapshot,
                                             const device_network_profile_t *network)
{
    if (!snapshot)
        return;

    if (!network_transport_wifi_enabled(network))
    {
        snapshot->wifi_ap_up = false;
        snapshot->wifi_sta_up = false;
        snapshot->wifi_ap_ip_addr = 0U;
        snapshot->wifi_ap_netmask_addr = 0U;
        snapshot->wifi_sta_ip_addr = 0U;
        snapshot->wifi_sta_netmask_addr = 0U;
    }

    if (!network_transport_ethernet_enabled(network))
    {
        snapshot->ethernet_up = false;
        snapshot->ethernet_ip_addr = 0U;
        snapshot->ethernet_netmask_addr = 0U;
    }

    if (snapshot->ethernet_up)
        snapshot->active_link = NETWORK_READY_LINK_ETHERNET;
    else if (snapshot->wifi_sta_up)
        snapshot->active_link = NETWORK_READY_LINK_WIFI_STA;
    else if (snapshot->wifi_ap_up)
        snapshot->active_link = NETWORK_READY_LINK_WIFI_AP;
    else
        snapshot->active_link = NETWORK_READY_LINK_NONE;

    snapshot->ready = (snapshot->active_link != NETWORK_READY_LINK_NONE);

    switch (snapshot->active_link)
    {
        case NETWORK_READY_LINK_ETHERNET:
            snapshot->active_ip_addr = snapshot->ethernet_ip_addr;
            snapshot->active_netmask_addr = snapshot->ethernet_netmask_addr;
            break;
        case NETWORK_READY_LINK_WIFI_STA:
            snapshot->active_ip_addr = snapshot->wifi_sta_ip_addr;
            snapshot->active_netmask_addr = snapshot->wifi_sta_netmask_addr;
            break;
        case NETWORK_READY_LINK_WIFI_AP:
            snapshot->active_ip_addr = snapshot->wifi_ap_ip_addr;
            snapshot->active_netmask_addr = snapshot->wifi_ap_netmask_addr;
            break;
        case NETWORK_READY_LINK_NONE:
        default:
            snapshot->active_ip_addr = 0U;
            snapshot->active_netmask_addr = 0U;
            break;
    }
}

static const char *network_status_name(const network_ready_snapshot_t *snapshot)
{
    if (!snapshot)
        return "none";

    return network_ready_link_name(snapshot->active_link);
}

static void network_status_addr_text(uint32_t ip_addr,
                                     char *buf,
                                     size_t buf_size)
{
    ip4_addr_t ip = {0};

    if (!buf || buf_size == 0U)
        return;

    buf[0] = '\0';

    if (ip_addr == 0U)
        return;

    ip.addr = ip_addr;
    ip4addr_ntoa_r(&ip, buf, buf_size);
}

static size_t build_failsafe_json(char *buf, size_t buf_size)
{
    size_t offset = 0U;
    int count;

    if (!buf || buf_size == 0U)
        return 0U;

    buf[0] = '\0';
    count = failsafe_export(failsafe_status_snapshot, FAILSAFE_MAX_OUTPUTS);

    if (!append_format(buf,
                       buf_size,
                       &offset,
                       "{\"ok\":true,\"count\":%d,\"outputs\":[",
                       count))
        return 0U;

    for (int i = 0; i < count; i++)
    {
        const failsafe_output_status_t *status = &failsafe_status_snapshot[i];

        if (i > 0 && !append_text(buf, buf_size, &offset, ","))
            return 0U;

        if (!append_format(buf,
                           buf_size,
                           &offset,
                           "{\"output_id\":%u,\"enabled\":%u,\"boot_action\":",
                           status->output_id,
                           status->enabled ? 1U : 0U))
            return 0U;

        if (!append_json_string(buf, buf_size, &offset, failsafe_action_to_code(status->boot_action)))
            return 0U;

        if (!append_text(buf, buf_size, &offset, ",\"comm_loss_action\":"))
            return 0U;

        if (!append_json_string(buf, buf_size, &offset, failsafe_action_to_code(status->comm_loss_action)))
            return 0U;

        if (!append_text(buf, buf_size, &offset, ",\"runtime_fault_action\":"))
            return 0U;

        if (!append_json_string(buf, buf_size, &offset, failsafe_action_to_code(status->runtime_fault_action)))
            return 0U;

        if (!append_format(buf,
                           buf_size,
                           &offset,
                           ",\"safe_value\":%" PRId32 ",\"recovery_mode\":",
                           status->safe_value))
            return 0U;

        if (!append_json_string(buf, buf_size, &offset, failsafe_recovery_to_code(status->recovery_mode)))
            return 0U;

        if (!append_format(buf,
                           buf_size,
                           &offset,
                           ",\"manual_reset_required\":%u,\"failsafe_active\":%u,"
                           "\"last_applied_value\":%" PRId32 ",\"last_reason\":",
                           status->manual_reset_required ? 1U : 0U,
                           status->failsafe_active ? 1U : 0U,
                           status->last_applied_value))
            return 0U;

        if (!append_json_string(buf, buf_size, &offset, failsafe_reason_name(status->last_reason)))
            return 0U;

        if (!append_format(buf,
                           buf_size,
                           &offset,
                           ",\"startup_mode\":"))
            return 0U;

        if (!append_json_string(buf, buf_size, &offset, failsafe_mode_to_code(status->startup_mode)))
            return 0U;

        if (!append_text(buf, buf_size, &offset, ",\"comm_loss_mode\":"))
            return 0U;

        if (!append_json_string(buf, buf_size, &offset, failsafe_mode_to_code(status->comm_loss_mode)))
            return 0U;

        if (!append_format(buf,
                           buf_size,
                           &offset,
                           ",\"manual_rearm\":%u,\"active\":%u,\"applied_value\":%" PRId32 ",\"reason\":",
                           status->manual_rearm ? 1U : 0U,
                           status->active ? 1U : 0U,
                           status->applied_value))
            return 0U;

        if (!append_json_string(buf, buf_size, &offset, status->reason))
            return 0U;

        if (!append_format(buf,
                           buf_size,
                           &offset,
                           ",\"safe_value_legacy\":%" PRId32,
                           status->safe_value))
            return 0U;

        if (!append_text(buf, buf_size, &offset, "}"))
            return 0U;
    }

    if (!append_text(buf, buf_size, &offset, "]}"))
        return 0U;

    return offset;
}

static size_t build_public_status_json(char *buf, size_t buf_size)
{
    size_t offset = 0U;
    cluster_metrics_t metrics = cluster_get_metrics();
    network_ready_snapshot_t net = {0};
    const char *transport_name = cluster_transport_active_name();
    int input_count = io_binding_export_inputs(input_profile_snapshot, IO_BINDING_MAX_INPUTS);
    int output_count = io_binding_export_outputs(status_output_snapshot, IO_BINDING_MAX_OUTPUTS);

    if (!buf || buf_size == 0U)
        return 0U;

    buf[0] = '\0';
    network_ready_get_snapshot(&net);

    char net_wifi_sta_ip_text[20] = {0};
    network_status_addr_text(net.wifi_sta_ip_addr, net_wifi_sta_ip_text, sizeof(net_wifi_sta_ip_text));

    if (!append_format(buf,
                       buf_size,
                       &offset,
                       "{\"node_id\":%" PRIu32 ",\"transport_name\":",
                       metrics.self_node))
        return 0U;

    if (!append_json_string(buf, buf_size, &offset, transport_name ? transport_name : "none"))
        return 0U;

    if (!append_format(buf,
                       buf_size,
                       &offset,
                       ",\"net_ready\":%u,\"operational_state\":\"online\",\"inputs\":[",
                       net.ready ? 1U : 0U))
        return 0U;

    for (int i = 0; i < input_count; i++)
    {
        const io_binding_input_view_t *input = &input_profile_snapshot[i];
        int32_t value = 0;

        if (!input || input->id == 0U)
            continue;

        if (i > 0 && !append_text(buf, buf_size, &offset, ","))
            return 0U;

        state_get_int(input->id, &value);
        if (!append_format(buf,
                           buf_size,
                           &offset,
                           "{\"id\":%u,\"value\":%" PRId32 "}",
                           input->id,
                           value))
            return 0U;
    }

    if (!append_text(buf, buf_size, &offset, "],\"outputs\":["))
        return 0U;

    for (int i = 0; i < output_count; i++)
    {
        const io_binding_output_view_t *output = &status_output_snapshot[i];
        failsafe_output_status_t failsafe_status = {0};
        bool has_failsafe;
        int32_t value = 0;

        if (!output || output->id == 0U)
            continue;

        if (i > 0 && !append_text(buf, buf_size, &offset, ","))
            return 0U;

        state_get_int(output->id, &value);
        has_failsafe = failsafe_get_policy(output->id, &failsafe_status);

        if (!append_format(buf,
                           buf_size,
                           &offset,
                           "{\"id\":%u,\"value\":%" PRId32 ",\"local\":%u,\"owner\":%" PRIu32
                           ",\"original\":%" PRIu32 ",\"failsafe_startup_mode\":",
                           output->id,
                           value,
                           cluster_io_is_local(output->id) ? 1U : 0U,
                           cluster_io_get_owner(output->id),
                           cluster_io_get_original_owner(output->id)))
            return 0U;

        if (!append_json_string(buf,
                                buf_size,
                                &offset,
                                has_failsafe ? failsafe_mode_to_code(failsafe_status.startup_mode) : "unknown"))
            return 0U;

        if (!append_text(buf, buf_size, &offset, ",\"failsafe_comm_loss_mode\":"))
            return 0U;

        if (!append_json_string(buf,
                                buf_size,
                                &offset,
                                has_failsafe ? failsafe_mode_to_code(failsafe_status.comm_loss_mode) : "unknown"))
            return 0U;

        if (!append_format(buf,
                           buf_size,
                           &offset,
                           ",\"failsafe_safe_value\":%" PRId32 ",\"failsafe_manual_rearm\":%u,"
                           "\"failsafe_active\":%u,\"failsafe_applied_value\":%" PRId32 ",\"effective_origin\":",
                           has_failsafe ? failsafe_status.safe_value : 0,
                           (has_failsafe && failsafe_status.manual_rearm) ? 1U : 0U,
                           (has_failsafe && failsafe_status.active) ? 1U : 0U,
                           has_failsafe ? failsafe_status.applied_value : value))
            return 0U;

        if (!append_json_string(buf,
                                buf_size,
                                &offset,
                                (has_failsafe && failsafe_status.active) ? "fail-safe" : "runtime"))
            return 0U;

        if (!append_text(buf, buf_size, &offset, ",\"failsafe_reason\":"))
            return 0U;

        if (!append_json_string(buf,
                                buf_size,
                                &offset,
                                has_failsafe ? failsafe_status.reason : "none"))
            return 0U;

        if (!append_format(buf,
                           buf_size,
                           &offset,
                           ",\"failsafe_enabled\":%u,\"failsafe_boot_action\":",
                           (has_failsafe && failsafe_status.enabled) ? 1U : 0U))
            return 0U;

        if (!append_json_string(buf,
                                buf_size,
                                &offset,
                                has_failsafe ? failsafe_action_to_code(failsafe_status.boot_action) : "unknown"))
            return 0U;

        if (!append_text(buf, buf_size, &offset, ",\"failsafe_comm_loss_action\":"))
            return 0U;

        if (!append_json_string(buf,
                                buf_size,
                                &offset,
                                has_failsafe ? failsafe_action_to_code(failsafe_status.comm_loss_action) : "unknown"))
            return 0U;

        if (!append_text(buf, buf_size, &offset, ",\"failsafe_runtime_fault_action\":"))
            return 0U;

        if (!append_json_string(buf,
                                buf_size,
                                &offset,
                                has_failsafe ? failsafe_action_to_code(failsafe_status.runtime_fault_action) : "unknown"))
            return 0U;

        if (!append_text(buf, buf_size, &offset, ",\"failsafe_recovery_mode\":"))
            return 0U;

        if (!append_json_string(buf,
                                buf_size,
                                &offset,
                                has_failsafe ? failsafe_recovery_to_code(failsafe_status.recovery_mode) : "unknown"))
            return 0U;

        if (!append_format(buf,
                           buf_size,
                           &offset,
                           ",\"failsafe_manual_reset_required\":%u,\"failsafe_last_reason\":",
                           (has_failsafe && failsafe_status.manual_reset_required) ? 1U : 0U))
            return 0U;

        if (!append_json_string(buf,
                                buf_size,
                                &offset,
                                has_failsafe ? failsafe_reason_name(failsafe_status.last_reason) : "none"))
            return 0U;

    if (!append_text(buf, buf_size, &offset, "}"))
        return 0U;
    }

    if (!append_text(buf, buf_size, &offset, "],\"net_wifi_sta_ip_text\":"))
        return 0U;

    if (!append_json_string(buf, buf_size, &offset, net_wifi_sta_ip_text[0] ? net_wifi_sta_ip_text : ""))
        return 0U;

    if (!append_text(buf, buf_size, &offset, "}"))
        return 0U;

    return offset;
}

static size_t build_status_json(char *buf, size_t buf_size)
{
    size_t offset = 0;
    int32_t i0 = 0, i1 = 0;
    int32_t o0 = 0, o1 = 0, o2 = 0;
    cluster_metrics_t metrics = cluster_get_metrics();
    kernel_metrics_snapshot_t kernel = {0};
    kernel_phase_metrics_t phase = {0};
    phase_monitor_snapshot_t phase_mon = {0};
    phase_load_test_snapshot_t load_test = {0};
    bus_health_metrics_t bus = {0};
    input_learning_snapshot_t learn = {0};
    network_ready_snapshot_t net = {0};
    wifi_manager_status_t wifi = {0};
    const device_network_profile_t *network_cfg = device_profile_network();
    rs485_engine_metrics_t rs485_engine_metrics = {0};
    rs485_master_metrics_t rs485_master_metrics = {0};
    rs485_hal_metrics_t rs485_hal_metrics = {0};
    const char *test_phase = cluster_self_test_phase();
    const char *net_name;
    const char *wifi_state_name;
    const char *rs485_link_state;
    const char *rs485_comm_state;
    const char *operational_state = "online";
    const char *main_issue = "Nenhum problema operacional relevante";
    const char *issue_origin = "runtime";
    const char *recommended_action = "Nenhuma acao imediata e necessaria.";
    bool wifi_enabled;
    bool ethernet_enabled;
    bool rs485_enabled;
    uint64_t uptime_ms = (uint64_t)(esp_timer_get_time() / 1000ULL);
    uint32_t deadline_miss_recent = 0U;
    uint32_t loop_fault_active = 0U;

    if (uptime_ms < status_prev_uptime_ms)
    {
        status_prev_deadline_miss = 0U;
    }

    if (kernel.deadline_miss >= status_prev_deadline_miss)
        deadline_miss_recent = kernel.deadline_miss - status_prev_deadline_miss;
    else
        deadline_miss_recent = kernel.deadline_miss;

    status_prev_deadline_miss = kernel.deadline_miss;
    status_prev_uptime_ms = uptime_ms;
    loop_fault_active = (kernel.max_exec_time > 1000ULL || kernel.overrun_count > 0U) ? 1U : 0U;
    uint32_t phase_fieldbus_max = 0U;
    uint32_t phase_fieldbus_deadline = 0U;
    uint32_t phase_fieldbus_overruns = 0U;
    uint32_t load_test_fieldbus_us = 0U;
    uint32_t test_running = cluster_self_test_is_running() ? 1U : 0U;
    uint32_t test_available = cluster_self_test_available() ? 1U : 0U;
    uint32_t automation_count = (uint32_t)automation_engine_get_node_count();
    uint32_t automation_saved = automation_engine_has_persisted_config() ? 1U : 0U;
    uint32_t bus_selftest = 0U;
    uint32_t rs485_online = 0U;
    uint32_t transport_ready = cluster_transport_is_ready() ? 1U : 0U;
    uint32_t transport_type = (uint32_t)cluster_transport_active_type();
    const char *transport_name = cluster_transport_active_name();
    char net_ip_text[20] = {0};
    char net_wifi_ap_ip_text[20] = {0};
    char net_wifi_sta_ip_text[20] = {0};
    char net_ethernet_ip_text[20] = {0};
    int input_count = io_binding_export_inputs(input_profile_snapshot, IO_BINDING_MAX_INPUTS);
    int output_count = io_binding_export_outputs(status_output_snapshot, IO_BINDING_MAX_OUTPUTS);
    uint16_t legacy_input_ids[2] = {0U, 0U};
    uint16_t legacy_output_ids[3] = {0U, 0U, 0U};
    int input_diag_count;
    uint32_t i10_raw = 0, i10_stable = 0, i10_noise = 0, i10_recent_noise = 0;
    uint32_t i11_raw = 0, i11_stable = 0, i11_noise = 0, i11_recent_noise = 0;
    uint32_t o100_local = 1U, o101_local = 1U, o102_local = 1U;
    uint32_t o100_owner = 0U, o101_owner = 0U, o102_owner = 0U;
    uint32_t o100_original = 0U, o101_original = 0U, o102_original = 0U;

    if (!buf || buf_size == 0U)
        return 0;

    buf[0] = '\0';

    legacy_input_ids[0] = device_profile_input_id_at(0);
    legacy_input_ids[1] = device_profile_input_id_at(1);
    legacy_output_ids[0] = device_profile_output_id_at(0);
    legacy_output_ids[1] = device_profile_output_id_at(1);
    legacy_output_ids[2] = device_profile_output_id_at(2);

    if (legacy_input_ids[0] != 0U)
        state_get_int(legacy_input_ids[0], &i0);
    if (legacy_input_ids[1] != 0U)
        state_get_int(legacy_input_ids[1], &i1);
    if (legacy_output_ids[0] != 0U)
    {
        state_get_int(legacy_output_ids[0], &o0);
        o100_local = cluster_io_is_local(legacy_output_ids[0]) ? 1U : 0U;
        o100_owner = cluster_io_get_owner(legacy_output_ids[0]);
        o100_original = cluster_io_get_original_owner(legacy_output_ids[0]);
    }
    if (legacy_output_ids[1] != 0U)
    {
        state_get_int(legacy_output_ids[1], &o1);
        o101_local = cluster_io_is_local(legacy_output_ids[1]) ? 1U : 0U;
        o101_owner = cluster_io_get_owner(legacy_output_ids[1]);
        o101_original = cluster_io_get_original_owner(legacy_output_ids[1]);
    }
    if (legacy_output_ids[2] != 0U)
    {
        state_get_int(legacy_output_ids[2], &o2);
        o102_local = cluster_io_is_local(legacy_output_ids[2]) ? 1U : 0U;
        o102_owner = cluster_io_get_owner(legacy_output_ids[2]);
        o102_original = cluster_io_get_original_owner(legacy_output_ids[2]);
    }
    kernel_metrics_get(&kernel);
    kernel_phase_metrics_get(&phase);
    phase_monitor_get(&phase_mon);
    phase_load_test_get(&load_test);
    bus_health_get(&bus);
    input_learning_get_snapshot(&learn);
    network_ready_get_snapshot(&net);
    network_status_sanitize_snapshot(&net, network_cfg);
    wifi_enabled = network_transport_wifi_enabled(network_cfg);
    ethernet_enabled = network_transport_ethernet_enabled(network_cfg);
    rs485_enabled = network_transport_rs485_enabled(network_cfg);
    phase_fieldbus_max = rs485_enabled ? phase.fieldbus_max : 0U;
    phase_fieldbus_deadline = rs485_enabled ? phase_mon.fieldbus_deadline_us : 0U;
    phase_fieldbus_overruns = rs485_enabled ? phase_mon.fieldbus_overruns : 0U;
    load_test_fieldbus_us = rs485_enabled ? load_test.fieldbus_us : 0U;

    if (wifi_enabled)
        wifi_manager_get_status(&wifi);

    if (rs485_enabled)
    {
        rs485_engine_get_metrics(&rs485_engine_metrics);
        rs485_master_get_metrics(&rs485_master_metrics);
        rs485_get_metrics(&rs485_hal_metrics);
        bus_selftest = rs485_engine_self_test_enabled() ? 1U : 0U;
    }

    if (!cluster_transport_visible((cluster_transport_type_t)transport_type, network_cfg))
    {
        transport_ready = 0U;
        transport_type = (uint32_t)CLUSTER_TRANSPORT_NONE;
        transport_name = "none";
    }

    net_name = network_status_name(&net);
    wifi_state_name = wifi_enabled ? wifi_manager_state_name(wifi.state) : "disabled";

    if (rs485_enabled)
    {
        rs485_online = (uint32_t)rs485_master_metrics.online_nodes;
        rs485_link_state = rs485_link_state_name(&rs485_engine_metrics,
                                                 &rs485_master_metrics,
                                                 &rs485_hal_metrics);
        rs485_comm_state = rs485_comm_state_name(&rs485_engine_metrics,
                                                 &rs485_master_metrics);
    }
    else
    {
        memset(&bus, 0, sizeof(bus));
        rs485_online = 0U;
        rs485_link_state = "disabled";
        rs485_comm_state = "disabled";
    }

    if (loop_fault_active || deadline_miss_recent > 0U)
    {
        operational_state = "degraded";
        main_issue = "Loop deterministico fora da margem";
        issue_origin = "kernel/control-loop";
        recommended_action = "Reduza a carga e revise a fase critica reportada no painel tecnico antes de expandir a operacao.";
    }
    else if (rs485_enabled && (bus.timeouts > 0U || bus.retries > 0U))
    {
        operational_state = "degraded";
        main_issue = "Barramento RS485 com falhas";
        issue_origin = "runtime/fieldbus";
        recommended_action = "Valide transceptor, terminacao e qualidade do enlace antes de manter o barramento em producao.";
    }
    else if ((wifi_enabled || ethernet_enabled) && !net.ready)
    {
        operational_state = "suspect";
        main_issue = "Transporte configurado sem enlace ativo";
        issue_origin = "runtime/network";
        recommended_action = "Revise credenciais, cabeamento e o transporte habilitado neste no.";
    }
    else if (metrics.active && (metrics.suspect > 0U || metrics.offline > 0U))
    {
        operational_state = "degraded";
        main_issue = "Cluster distribuido degradado";
        issue_origin = "cluster";
        recommended_action = "Revise os nos suspect/offline e confirme o enlace principal do cluster.";
    }

    network_status_addr_text(net.active_ip_addr, net_ip_text, sizeof(net_ip_text));
    network_status_addr_text(net.wifi_ap_ip_addr, net_wifi_ap_ip_text, sizeof(net_wifi_ap_ip_text));
    network_status_addr_text(net.wifi_sta_ip_addr, net_wifi_sta_ip_text, sizeof(net_wifi_sta_ip_text));
    network_status_addr_text(net.ethernet_ip_addr, net_ethernet_ip_text, sizeof(net_ethernet_ip_text));
    input_diag_count = io_driver_get_input_diag(status_input_diag_snapshot, STATUS_IO_MAX_CHANNELS);

    for (int idx = 0; idx < input_diag_count; idx++)
    {
        if (legacy_input_ids[0] != 0U && status_input_diag_snapshot[idx].id == legacy_input_ids[0])
        {
            i10_raw = status_input_diag_snapshot[idx].raw_edges;
            i10_stable = status_input_diag_snapshot[idx].stable_edges;
            i10_noise = status_input_diag_snapshot[idx].noise_edges;
            i10_recent_noise = status_input_diag_snapshot[idx].recent_noise_edges;
        }
        else if (legacy_input_ids[1] != 0U && status_input_diag_snapshot[idx].id == legacy_input_ids[1])
        {
            i11_raw = status_input_diag_snapshot[idx].raw_edges;
            i11_stable = status_input_diag_snapshot[idx].stable_edges;
            i11_noise = status_input_diag_snapshot[idx].noise_edges;
            i11_recent_noise = status_input_diag_snapshot[idx].recent_noise_edges;
        }
    }

    if (!append_format(buf,
        buf_size,
        &offset,
        "{\"i0\":%" PRId32 ",\"i1\":%" PRId32 ",\"o0\":%" PRId32 ",\"o1\":%" PRId32 ",\"o2\":%" PRId32 ","
        "\"o100_local\":%" PRIu32 ",\"o101_local\":%" PRIu32 ",\"o102_local\":%" PRIu32 ","
        "\"o100_owner\":%" PRIu32 ",\"o101_owner\":%" PRIu32 ",\"o102_owner\":%" PRIu32 ","
        "\"o100_original\":%" PRIu32 ",\"o101_original\":%" PRIu32 ",\"o102_original\":%" PRIu32 ","
        "\"uptime_ms\":%" PRIu64 ","
        "\"a_count\":%" PRIu32 ",\"a_saved\":%" PRIu32 ","
        "\"k_jitter_max\":%" PRIu64 ",\"k_exec_max\":%" PRIu64 ",\"k_deadline_miss\":%" PRIu32 ",\"k_deadline_miss_recent\":%" PRIu32 ",\"k_overrun\":%" PRIu32 ",\"k_loop_fault\":%" PRIu32 ","
        "\"p_io\":%" PRIu32 ",\"p_fieldbus\":%" PRIu32 ",\"p_automation\":%" PRIu32 ",\"p_events\":%" PRIu32 ","
        "\"p_io_deadline\":%" PRIu32 ",\"p_io_apply_deadline\":%" PRIu32 ",\"p_fieldbus_deadline\":%" PRIu32 ","
        "\"p_automation_deadline\":%" PRIu32 ",\"p_events_deadline\":%" PRIu32 ","
        "\"p_io_overruns\":%" PRIu32 ",\"p_io_apply_overruns\":%" PRIu32 ",\"p_fieldbus_overruns\":%" PRIu32 ","
        "\"p_automation_overruns\":%" PRIu32 ",\"p_events_overruns\":%" PRIu32 ","
        "\"lt_active\":%u,\"lt_phase_count\":%" PRIu32 ",\"lt_total_us\":%" PRIu32 ","
        "\"lt_io_us\":%" PRIu32 ",\"lt_io_apply_us\":%" PRIu32 ",\"lt_fieldbus_us\":%" PRIu32 ","
        "\"lt_automation_us\":%" PRIu32 ",\"lt_events_us\":%" PRIu32 ","
        "\"b_crc\":%" PRIu32 ",\"b_timeouts\":%" PRIu32 ",\"b_retries\":%" PRIu32 ",\"b_avg_lat\":%" PRIu32 ",\"b_max_lat\":%" PRIu32 ",\"b_selftest\":%" PRIu32 ","
        "\"i10_raw\":%" PRIu32 ",\"i10_stable\":%" PRIu32 ",\"i10_noise\":%" PRIu32 ",\"i10_recent_noise\":%" PRIu32 ","
        "\"i11_raw\":%" PRIu32 ",\"i11_stable\":%" PRIu32 ",\"i11_noise\":%" PRIu32 ",\"i11_recent_noise\":%" PRIu32 ","
        "\"learn_armed\":%" PRIu32 ",\"learn_found\":%" PRIu32 ",\"learn_input\":%" PRIu32 ","
        "\"transport_ready\":%" PRIu32 ",\"transport_type\":%" PRIu32 ",\"transport_name\":",
        i0, i1, o0, o1, o2,
        o100_local, o101_local, o102_local,
        o100_owner, o101_owner, o102_owner,
        o100_original, o101_original, o102_original,
        automation_count, automation_saved,
        kernel.max_jitter, kernel.max_exec_time, kernel.deadline_miss, deadline_miss_recent, kernel.overrun_count, loop_fault_active,
        uptime_ms,
        phase.io_max, phase_fieldbus_max, phase.automation_max, phase.events_max,
        phase_mon.io_deadline_us, phase_mon.io_apply_deadline_us, phase_fieldbus_deadline,
        phase_mon.automation_deadline_us, phase_mon.events_deadline_us,
        phase_mon.io_overruns, phase_mon.io_apply_overruns, phase_fieldbus_overruns,
        phase_mon.automation_overruns, phase_mon.events_overruns,
        load_test.active ? 1U : 0U, (uint32_t)load_test.active_phase_count, load_test.total_us,
        load_test.io_us, load_test.io_apply_us, load_test_fieldbus_us,
        load_test.automation_us, load_test.events_us,
        bus.crc_errors, bus.timeouts, bus.retries, bus.avg_latency_us, bus.max_latency_us, bus_selftest,
        i10_raw, i10_stable, i10_noise, i10_recent_noise,
        i11_raw, i11_stable, i11_noise, i11_recent_noise,
        (uint32_t)(learn.armed ? 1U : 0U),
        (uint32_t)(learn.found ? 1U : 0U),
        (uint32_t)learn.input_id,
        transport_ready, transport_type))
        return 0;

    if (!append_json_string(buf, buf_size, &offset, transport_name))
        return 0;

    if (!append_format(buf,
        buf_size,
        &offset,
        ",\"net_ready\":%" PRIu32 ",\"net_active_link\":%" PRIu32 ",\"net_wifi_ap\":%" PRIu32
        ",\"net_wifi_sta\":%" PRIu32 ",\"net_ethernet\":%" PRIu32 ",\"net_active_ip\":%" PRIu32
        ",\"wifi_enabled\":%" PRIu32 ",\"ethernet_enabled\":%" PRIu32 ",\"rs485_enabled\":%" PRIu32
        ",\"rs485_online\":%" PRIu32
        ",\"rs485_tx_count\":%" PRIu32 ",\"rs485_rx_count\":%" PRIu32
        ",\"rs485_timeout_count\":%" PRIu32 ",\"rs485_crc_error_count\":%" PRIu32
        ",\"rs485_format_error_count\":%" PRIu32 ",\"rs485_retry_count\":%" PRIu32
        ",\"rs485_rx_ignored_count\":%" PRIu32 ",\"rs485_last_seen_ms\":%" PRIu32
        ",\"rs485_last_tx_ms\":%" PRIu32 ",\"rs485_selftest_enabled\":%" PRIu32
        ",\"rs485_selftest_active\":%" PRIu32 ",\"rs485_hal_tx_bytes\":%" PRIu32
        ",\"rs485_hal_rx_bytes\":%" PRIu32 ",\"rs485_hal_drop_bytes\":%" PRIu32
        ",\"rs485_registry_consumed\":0,\"wifi_initialized\":%" PRIu32
        ",\"wifi_credentials_saved\":%" PRIu32 ",\"wifi_sta_requested\":%" PRIu32
        ",\"wifi_sta_connected\":%" PRIu32 ",\"wifi_ap_fallback\":%" PRIu32
        ",\"wifi_retry_count\":%u,\"wifi_retry_limit\":%u,\"wifi_disconnect_reason\":%u"
        ",\"wifi_state_code\":%u,\"net_active_name\":",
        net.ready ? 1U : 0U,
        (uint32_t)net.active_link,
        net.wifi_ap_up ? 1U : 0U,
        net.wifi_sta_up ? 1U : 0U,
        net.ethernet_up ? 1U : 0U,
        net.active_ip_addr,
        wifi_enabled ? 1U : 0U,
        ethernet_enabled ? 1U : 0U,
        rs485_enabled ? 1U : 0U,
        rs485_online,
        rs485_master_metrics.tx_count,
        rs485_engine_metrics.rx_count,
        rs485_master_metrics.timeout_count,
        rs485_engine_metrics.crc_error_count,
        rs485_engine_metrics.format_error_count,
        rs485_master_metrics.retry_count,
        rs485_master_metrics.rx_ignored_count,
        rs485_master_metrics.last_ack_ms,
        rs485_master_metrics.last_tx_ms,
        rs485_engine_metrics.self_test_enabled ? 1U : 0U,
        rs485_engine_metrics.self_test_active ? 1U : 0U,
        rs485_hal_metrics.tx_bytes,
        rs485_hal_metrics.rx_bytes,
        rs485_hal_metrics.rx_dropped_bytes,
        wifi.initialized ? 1U : 0U,
        wifi.credentials_saved ? 1U : 0U,
        wifi.sta_requested ? 1U : 0U,
        wifi.sta_connected ? 1U : 0U,
        wifi.ap_fallback_active ? 1U : 0U,
        wifi.retry_count,
        wifi.retry_limit,
        wifi.last_disconnect_reason,
        (unsigned int)wifi.state))
        return 0;

    if (!append_json_string(buf, buf_size, &offset, net_name))
        return 0;

    if (!append_text(buf, buf_size, &offset, ",\"wifi_state\":"))
        return 0;

    if (!append_json_string(buf, buf_size, &offset, wifi_state_name))
        return 0;

    if (!append_text(buf, buf_size, &offset, ",\"rs485_link_state\":"))
        return 0;

    if (!append_json_string(buf, buf_size, &offset, rs485_link_state))
        return 0;

    if (!append_text(buf, buf_size, &offset, ",\"rs485_comm_state\":"))
        return 0;

    if (!append_json_string(buf, buf_size, &offset, rs485_comm_state))
        return 0;

    if (!append_text(buf, buf_size, &offset, ",\"rs485_transport_role\":"))
        return 0;

    if (!append_json_string(buf,
                            buf_size,
                            &offset,
                            rs485_enabled
                                ? ((cluster_transport_active_type() == CLUSTER_TRANSPORT_RS485)
                                       ? "cluster-fabric"
                                       : "cluster-standby")
                                : "disabled"))
        return 0;

    if (!append_text(buf, buf_size, &offset, ",\"wifi_ssid\":"))
        return 0;

    if (!append_json_string(buf, buf_size, &offset, wifi.ssid))
        return 0;

    if (!append_text(buf, buf_size, &offset, ",\"net_active_ip_text\":"))
        return 0;

    if (!append_json_string(buf, buf_size, &offset, net_ip_text))
        return 0;

    if (!append_text(buf, buf_size, &offset, ",\"net_wifi_ap_ip_text\":"))
        return 0;

    if (!append_json_string(buf, buf_size, &offset, net_wifi_ap_ip_text))
        return 0;

    if (!append_text(buf, buf_size, &offset, ",\"net_wifi_sta_ip_text\":"))
        return 0;

    if (!append_json_string(buf, buf_size, &offset, net_wifi_sta_ip_text))
        return 0;

    if (!append_text(buf, buf_size, &offset, ",\"net_ethernet_ip_text\":"))
        return 0;

    if (!append_json_string(buf, buf_size, &offset, net_ethernet_ip_text))
        return 0;

    if (!append_format(buf,
        buf_size,
        &offset,
        ",\"cluster_active\":%" PRIu32 ",\"c_total\":%" PRIu32 ",\"c_online\":%" PRIu32 ",\"c_suspect\":%" PRIu32 ","
        "\"c_offline\":%" PRIu32 ",\"c_health\":%" PRIu32 ",\"c_self\":%" PRIu32 ",\"c_master\":%" PRIu32 ","
        "\"c_test\":%" PRIu32 ",\"c_test_available\":%" PRIu32 ",\"c_test_phase\":",
        metrics.active, metrics.total_nodes, metrics.online, metrics.suspect,
        metrics.offline, metrics.avg_health, metrics.self_node, metrics.master_node,
        test_running, test_available))
        return 0;

    if (!append_json_string(buf, buf_size, &offset, test_phase))
        return 0;

    if (!append_text(buf, buf_size, &offset, ",\"learn_name\":"))
        return 0;

    if (!append_json_string(buf, buf_size, &offset, learn.input_name ? learn.input_name : ""))
        return 0;

    if (!append_text(buf, buf_size, &offset, ",\"operational_state\":"))
        return 0;

    if (!append_json_string(buf, buf_size, &offset, operational_state))
        return 0;

    if (!append_text(buf, buf_size, &offset, ",\"main_issue\":"))
        return 0;

    if (!append_json_string(buf, buf_size, &offset, main_issue))
        return 0;

    if (!append_text(buf, buf_size, &offset, ",\"issue_origin\":"))
        return 0;

    if (!append_json_string(buf, buf_size, &offset, issue_origin))
        return 0;

    if (!append_text(buf, buf_size, &offset, ",\"recommended_action\":"))
        return 0;

    if (!append_json_string(buf, buf_size, &offset, recommended_action))
        return 0;

    if (!append_text(buf, buf_size, &offset, ",\"inputs\":["))
        return 0;

    for (int i = 0; i < input_count; i++)
    {
        const io_binding_input_view_t *binding = &input_profile_snapshot[i];
        const device_input_profile_t *input = device_profile_find_input(binding->id);
        const io_driver_input_diag_t *diag;
        char local_code[INSTALLATION_MAP_LOCAL_CODE_LEN];
        int32_t level = 0;

        if (!binding || binding->id == 0 || !input)
            continue;

        if (i > 0 && !append_text(buf, buf_size, &offset, ","))
            return 0;

        installation_local_code(true, i + 1, local_code);
        state_get_int(input->id, &level);
        diag = find_input_diag_by_id(input->id, input_diag_count);

        if (!append_format(buf, buf_size, &offset,
            "{\"id\":%u,\"local_index\":%d,\"local_code\":",
            binding->id,
            i + 1))
            return 0;

        if (!append_json_string(buf, buf_size, &offset, local_code))
            return 0;

        if (!append_format(buf, buf_size, &offset,
            ",\"gpio\":%d,\"active_low\":%u,\"value\":%" PRId32 ",\"debounce\":%u,\"raw\":%" PRIu32
            ",\"stable\":%" PRIu32 ",\"noise\":%" PRIu32 ",\"recent_raw\":%" PRIu32 ",\"recent_stable\":%" PRIu32
            ",\"recent_noise\":%" PRIu32 ",\"backend\":",
            binding->gpio,
            input->active_low ? 1U : 0U,
            level,
            input->debounce_samples,
            diag ? diag->raw_edges : 0U,
            diag ? diag->stable_edges : 0U,
            diag ? diag->noise_edges : 0U,
            diag ? diag->recent_raw_edges : 0U,
            diag ? diag->recent_stable_edges : 0U,
            diag ? diag->recent_noise_edges : 0U))
            return 0;

        if (!append_json_string(buf, buf_size, &offset, io_binding_backend_code(binding->backend)))
            return 0;

        if (!append_text(buf, buf_size, &offset, ",\"backend_name\":"))
            return 0;

        if (!append_json_string(buf, buf_size, &offset, binding->backend_name))
            return 0;

        if (!append_format(buf, buf_size, &offset,
            ",\"backend_instance\":%d,\"endpoint_index\":%d,\"address\":",
            binding->backend_instance,
            binding->endpoint_index))
            return 0;

        if (!append_json_string(buf, buf_size, &offset, binding->address))
            return 0;

        if (!append_text(buf, buf_size, &offset, ",\"backend_address\":"))
            return 0;

        if (!append_json_string(buf, buf_size, &offset, binding->address))
            return 0;

        if (!append_text(buf, buf_size, &offset, ",\"display_name_local\":"))
            return 0;

        if (!append_json_string(buf, buf_size, &offset, binding->name))
            return 0;

        if (!append_text(buf, buf_size, &offset, ",\"name\":"))
            return 0;

        if (!append_json_string(buf, buf_size, &offset, binding->name))
            return 0;

        if (!append_text(buf, buf_size, &offset, ",\"role\":"))
            return 0;

        if (!append_json_string(buf, buf_size, &offset, binding->role))
            return 0;

        const plc_channel_desc_t *plc_in = find_plc_channel_desc(DEVICE_CHANNEL_CLASS_DIGITAL_INPUT, i);

        if (!append_text(buf, buf_size, &offset, ",\"description\":"))
            return 0;

        if (!append_json_string(buf, buf_size, &offset, binding->description))
            return 0;

        if (!append_text(buf, buf_size, &offset, ",\"plc_code\":"))
            return 0;

        if (!append_json_string(buf, buf_size, &offset, plc_in ? plc_in->plc_code : local_code))
            return 0;

        if (!append_text(buf, buf_size, &offset, ",\"plc_name\":"))
            return 0;

        if (!append_json_string(buf, buf_size, &offset, plc_in ? plc_in->default_name : binding->name))
            return 0;

        if (!append_format(buf, buf_size, &offset, ",\"plc_available\":%u}", (plc_in && plc_in->available) ? 1U : 0U))
            return 0;
    }

    if (!append_text(buf, buf_size, &offset, "],\"outputs\":["))
        return 0;

    for (int i = 0; i < output_count; i++)
    {
        const io_binding_output_view_t *output = &status_output_snapshot[i];
        const device_output_profile_t *profile = device_profile_find_output(output->id);
        const plc_channel_desc_t *plc_out = find_plc_channel_desc(DEVICE_CHANNEL_CLASS_DIGITAL_OUTPUT, i);
        failsafe_output_status_t failsafe_status = {0};
        bool has_failsafe;
        char local_code[INSTALLATION_MAP_LOCAL_CODE_LEN];
        int32_t value = 0;

        if (!output || output->id == 0 || !profile)
            continue;

        if (i > 0 && !append_text(buf, buf_size, &offset, ","))
            return 0;

        installation_local_code(false, i + 1, local_code);
        state_get_int(output->id, &value);
        has_failsafe = failsafe_get_policy(output->id, &failsafe_status);

        if (!append_format(buf, buf_size, &offset,
            "{\"id\":%u,\"local_index\":%d,\"local_code\":",
            output->id,
            i + 1))
            return 0;

        if (!append_json_string(buf, buf_size, &offset, local_code))
            return 0;

        if (!append_format(buf, buf_size, &offset,
            ",\"gpio\":%d,\"active_low\":%u,\"value\":%" PRId32 ",\"local\":%u,\"owner\":%" PRIu32
            ",\"original\":%" PRIu32 ",\"backend\":",
            output->gpio,
            profile->active_low ? 1U : 0U,
            value,
            cluster_io_is_local(output->id) ? 1U : 0U,
            cluster_io_get_owner(output->id),
            cluster_io_get_original_owner(output->id)))
            return 0;

        if (!append_json_string(buf, buf_size, &offset, io_binding_backend_code(output->backend)))
            return 0;

        if (!append_text(buf, buf_size, &offset, ",\"backend_name\":"))
            return 0;

        if (!append_json_string(buf, buf_size, &offset, output->backend_name))
            return 0;

        if (!append_format(buf, buf_size, &offset,
            ",\"backend_instance\":%d,\"endpoint_index\":%d,\"address\":",
            output->backend_instance,
            output->endpoint_index))
            return 0;

        if (!append_json_string(buf, buf_size, &offset, output->address))
            return 0;

        if (!append_text(buf, buf_size, &offset, ",\"backend_address\":"))
            return 0;

        if (!append_json_string(buf, buf_size, &offset, output->address))
            return 0;

        if (!append_text(buf, buf_size, &offset, ",\"display_name_local\":"))
            return 0;

        if (!append_json_string(buf, buf_size, &offset, output->name))
            return 0;

        if (!append_text(buf, buf_size, &offset, ",\"name\":"))
            return 0;

        if (!append_json_string(buf, buf_size, &offset, output->name))
            return 0;

        if (!append_text(buf, buf_size, &offset, ",\"role\":"))
            return 0;

        if (!append_json_string(buf, buf_size, &offset, output->role))
            return 0;

        if (!append_text(buf, buf_size, &offset, ",\"description\":"))
            return 0;

        if (!append_json_string(buf, buf_size, &offset, output->description))
            return 0;

        if (!append_text(buf, buf_size, &offset, ",\"plc_code\":"))
            return 0;

        if (!append_json_string(buf, buf_size, &offset, plc_out ? plc_out->plc_code : local_code))
            return 0;

        if (!append_text(buf, buf_size, &offset, ",\"plc_name\":"))
            return 0;

        if (!append_json_string(buf, buf_size, &offset, plc_out ? plc_out->default_name : output->name))
            return 0;

        if (!append_format(buf, buf_size, &offset, ",\"plc_available\":%u", (plc_out && plc_out->available) ? 1U : 0U))
            return 0;

        if (!append_format(buf,
                           buf_size,
                           &offset,
                           ",\"failsafe_startup_mode\":"))
            return 0;

        if (!append_json_string(buf,
                                buf_size,
                                &offset,
                                has_failsafe ? failsafe_mode_to_code(failsafe_status.startup_mode) : "unknown"))
            return 0;

        if (!append_text(buf, buf_size, &offset, ",\"failsafe_comm_loss_mode\":"))
            return 0;

        if (!append_json_string(buf,
                                buf_size,
                                &offset,
                                has_failsafe ? failsafe_mode_to_code(failsafe_status.comm_loss_mode) : "unknown"))
            return 0;

        if (!append_format(buf,
                           buf_size,
                           &offset,
                           ",\"failsafe_safe_value\":%" PRId32 ",\"failsafe_manual_rearm\":%u,"
                           "\"failsafe_active\":%u,\"failsafe_applied_value\":%" PRId32 ",\"effective_origin\":",
                           has_failsafe ? failsafe_status.safe_value : 0,
                           (has_failsafe && failsafe_status.manual_rearm) ? 1U : 0U,
                           (has_failsafe && failsafe_status.active) ? 1U : 0U,
                           has_failsafe ? failsafe_status.applied_value : value))
            return 0;

        if (!append_json_string(buf,
                                buf_size,
                                &offset,
                                (has_failsafe && failsafe_status.active) ? "fail-safe" : "runtime"))
            return 0;

        if (!append_text(buf, buf_size, &offset, ",\"failsafe_reason\":"))
            return 0;

        if (!append_json_string(buf,
                                buf_size,
                                &offset,
                                has_failsafe ? failsafe_status.reason : "none"))
            return 0;

        if (!append_format(buf,
                           buf_size,
                           &offset,
                           ",\"failsafe_enabled\":%u,\"failsafe_boot_action\":",
                           (has_failsafe && failsafe_status.enabled) ? 1U : 0U))
            return 0;

        if (!append_json_string(buf,
                                buf_size,
                                &offset,
                                has_failsafe ? failsafe_action_to_code(failsafe_status.boot_action) : "unknown"))
            return 0;

        if (!append_text(buf, buf_size, &offset, ",\"failsafe_comm_loss_action\":"))
            return 0;

        if (!append_json_string(buf,
                                buf_size,
                                &offset,
                                has_failsafe ? failsafe_action_to_code(failsafe_status.comm_loss_action) : "unknown"))
            return 0;

        if (!append_text(buf, buf_size, &offset, ",\"failsafe_runtime_fault_action\":"))
            return 0;

        if (!append_json_string(buf,
                                buf_size,
                                &offset,
                                has_failsafe ? failsafe_action_to_code(failsafe_status.runtime_fault_action) : "unknown"))
            return 0;

        if (!append_text(buf, buf_size, &offset, ",\"failsafe_recovery_mode\":"))
            return 0;

        if (!append_json_string(buf,
                                buf_size,
                                &offset,
                                has_failsafe ? failsafe_recovery_to_code(failsafe_status.recovery_mode) : "unknown"))
            return 0;

        if (!append_format(buf,
                           buf_size,
                           &offset,
                           ",\"failsafe_manual_reset_required\":%u,\"failsafe_last_reason\":",
                           (has_failsafe && failsafe_status.manual_reset_required) ? 1U : 0U))
            return 0;

        if (!append_json_string(buf,
                                buf_size,
                                &offset,
                                has_failsafe ? failsafe_reason_name(failsafe_status.last_reason) : "none"))
            return 0;

        if (!append_text(buf, buf_size, &offset, "}"))
            return 0;
    }

    if (!append_text(buf, buf_size, &offset, "]}"))
        return 0;

    return offset;
}

static size_t build_automation_json(char *buf, size_t buf_size)
{
    size_t offset = 0U;
    int count;
    int diag_count;

    if (!buf || buf_size == 0U)
        return 0U;

    buf[0] = '\0';
    count = automation_engine_export_nodes(automation_rules_snapshot, AUTOMATION_ENGINE_MAX_NODES);
    diag_count = automation_engine_export_diags(automation_diag_snapshot, AUTOMATION_ENGINE_MAX_NODES);

    if (!append_format(buf,
                       buf_size,
                       &offset,
                       "{\"saved\":%u,\"count\":%d,\"rules\":[",
                       automation_engine_has_persisted_config() ? 1U : 0U,
                       count))
        return 0U;

    for (int i = 0; i < count; i++)
    {
        const automation_node_t *rule = &automation_rules_snapshot[i];
        const automation_rule_diag_t *diag = (i < diag_count) ? &automation_diag_snapshot[i] : NULL;

        if (i > 0 && !append_text(buf, buf_size, &offset, ","))
            return 0U;

        if (!append_format(buf,
                           buf_size,
                           &offset,
                           "{\"input\":%u,\"output\":%u,\"threshold\":%" PRId32
                           ",\"op\":",
                           rule->input,
                           rule->output,
                           rule->threshold))
            return 0U;

        if (!append_json_string(buf, buf_size, &offset, automation_engine_operator_to_code(rule->op)))
            return 0U;
        if (!append_text(buf, buf_size, &offset, ",\"op_symbol\":"))
            return 0U;
        if (!append_json_string(buf, buf_size, &offset, automation_engine_operator_to_symbol(rule->op)))
            return 0U;
        if (!append_text(buf, buf_size, &offset, ",\"mode\":"))
            return 0U;
        if (!append_json_string(buf, buf_size, &offset, automation_engine_mode_to_code(rule->mode)))
            return 0U;
        if (!append_text(buf, buf_size, &offset, ",\"mode_label\":"))
            return 0U;
        if (!append_json_string(buf, buf_size, &offset, automation_engine_mode_to_label(rule->mode)))
            return 0U;

        if (!append_format(buf,
                           buf_size,
                           &offset,
                           ",\"duration_ms\":%u,\"on_true\":%d,\"on_false\":%d"
                           ",\"last_trigger_ms\":%" PRIu32 ",\"last_eval_ms\":%" PRIu32
                           ",\"last_action_ms\":%" PRIu32 ",\"last_condition\":%u"
                           ",\"last_action_value\":%u,\"last_target_local\":%u"
                           ",\"last_target_owner\":%" PRIu32 ",\"last_target_original_owner\":%" PRIu32
                           ",\"last_action_result\":",
                           rule->duration_ms,
                           rule->on_true,
                           rule->on_false,
                           diag ? diag->last_trigger_ms : 0U,
                           diag ? diag->last_eval_ms : 0U,
                           diag ? diag->last_action_ms : 0U,
                           diag ? diag->last_condition : 0U,
                           diag ? diag->last_action_value : 0U,
                           diag ? diag->last_target_local : 0U,
                           diag ? diag->last_target_owner : 0U,
                           diag ? diag->last_target_original_owner : 0U))
            return 0U;

        if (!append_json_string(buf,
                                buf_size,
                                &offset,
                                automation_engine_action_result_name(diag ? diag->last_action_result : AUTOMATION_ACTION_IDLE)))
            return 0U;

        automation_interlock_t ilk = {0};
        automation_engine_get_interlock_at(i, &ilk);

        if (!append_format(buf,
                           buf_size,
                           &offset,
                           ",\"interlock\":{\"enabled\":%u,\"logic_op\":%u,\"cond_count\":%u,\"conditions\":[",
                           ilk.enabled ? 1U : 0U,
                           ilk.logic_op,
                           ilk.cond_count))
            return 0U;

        for (int c = 0; c < ilk.cond_count && c < AUTOMATION_MAX_INTERLOCK_COND; c++)
        {
            if (c > 0 && !append_text(buf, buf_size, &offset, ","))
                return 0U;

            if (!append_format(buf,
                               buf_size,
                               &offset,
                               "{\"channel\":%u,\"op\":%u,\"target_val\":%d}",
                               ilk.conditions[c].channel,
                               ilk.conditions[c].op,
                               ilk.conditions[c].target_val))
                return 0U;
        }

        if (!append_text(buf, buf_size, &offset, "]}}"))
            return 0U;
    }

    if (!append_text(buf, buf_size, &offset, "]}"))
        return 0U;

    return offset;
}


static size_t build_profile_json(char *buf, size_t buf_size)
{
    size_t offset = 0;
    int input_count = io_binding_export_inputs(input_profile_snapshot, IO_BINDING_MAX_INPUTS);
    int output_count = io_binding_export_outputs(output_profile_snapshot, IO_BINDING_MAX_OUTPUTS);
    const device_network_profile_t *network = device_profile_network();
    const device_network_w5500_profile_t *w5500 = device_profile_w5500();
    bool ethernet_configured = device_profile_w5500_is_configured();
    bool first = true;

    if (!buf || buf_size == 0U)
        return 0;

    buf[0] = '\0';

    if (!append_text(buf, buf_size, &offset, "{\"inputs\":["))
        return 0;

    for (int i = 0; i < input_count; i++)
    {
        const io_binding_input_view_t *binding = &input_profile_snapshot[i];
        const device_input_profile_t *input = device_profile_find_input(binding->id);
        char local_code[INSTALLATION_MAP_LOCAL_CODE_LEN];

        if (!binding || binding->id == 0 || !input)
            continue;

        installation_local_code(true, i + 1, local_code);

        if (!append_text(buf, buf_size, &offset, first ? "" : ","))
            return offset;

        if (!append_format(buf, buf_size, &offset, "{\"id\":%u,\"local_index\":%d,\"local_code\":", binding->id, i + 1))
            return offset;
        if (!append_json_string(buf, buf_size, &offset, local_code))
            return offset;
        if (!append_format(buf, buf_size, &offset, ",\"gpio\":%d,\"default_gpio\":%d,\"backend\":", binding->gpio, binding->default_gpio))
            return offset;
        if (!append_json_string(buf, buf_size, &offset, io_binding_backend_code(binding->backend)))
            return offset;
        if (!append_text(buf, buf_size, &offset, ",\"backend_name\":"))
            return offset;
        if (!append_json_string(buf, buf_size, &offset, binding->backend_name))
            return offset;
        if (!append_format(buf, buf_size, &offset, ",\"backend_instance\":%d,\"endpoint_index\":%d,\"address\":", binding->backend_instance, binding->endpoint_index))
            return offset;
        if (!append_json_string(buf, buf_size, &offset, binding->address))
            return offset;
        if (!append_text(buf, buf_size, &offset, ",\"backend_address\":"))
            return offset;
        if (!append_json_string(buf, buf_size, &offset, binding->address))
            return offset;
        if (!append_text(buf, buf_size, &offset, ",\"display_name_local\":"))
            return offset;
        if (!append_json_string(buf, buf_size, &offset, binding->name))
            return offset;
        if (!append_format(buf, buf_size, &offset, ",\"active_low\":%u,\"debounce\":%u,\"name\":", input->active_low ? 1U : 0U, input->debounce_samples))
            return offset;
        if (!append_json_string(buf, buf_size, &offset, binding->name))
            return offset;
        if (!append_text(buf, buf_size, &offset, ",\"role\":"))
            return offset;
        if (!append_json_string(buf, buf_size, &offset, binding->role))
            return offset;
        const plc_channel_desc_t *plc_in = find_plc_channel_desc(DEVICE_CHANNEL_CLASS_DIGITAL_INPUT, i);

        if (!append_text(buf, buf_size, &offset, ",\"description\":"))
            return offset;
        if (!append_json_string(buf, buf_size, &offset, binding->description))
            return offset;
        if (!append_text(buf, buf_size, &offset, ",\"plc_code\":"))
            return offset;
        if (!append_json_string(buf, buf_size, &offset, plc_in ? plc_in->plc_code : local_code))
            return offset;
        if (!append_text(buf, buf_size, &offset, ",\"plc_name\":"))
            return offset;
        if (!append_json_string(buf, buf_size, &offset, plc_in ? plc_in->default_name : binding->name))
            return offset;
        if (!append_format(buf, buf_size, &offset, ",\"plc_available\":%u}", (plc_in && plc_in->available) ? 1U : 0U))
            return offset;

        first = false;
    }

    if (!append_text(buf, buf_size, &offset, "],\"outputs\":["))
        return offset;
    first = true;

    for (int i = 0; i < output_count; i++)
    {
        const io_binding_output_view_t *output = &output_profile_snapshot[i];
        const device_output_profile_t *profile = device_profile_find_output(output->id);
        const plc_channel_desc_t *plc_out = find_plc_channel_desc(DEVICE_CHANNEL_CLASS_DIGITAL_OUTPUT, i);
        char local_code[INSTALLATION_MAP_LOCAL_CODE_LEN];

        if (!output || output->id == 0 || !profile)
            continue;

        installation_local_code(false, i + 1, local_code);

        if (!append_text(buf, buf_size, &offset, first ? "" : ","))
            return offset;

        if (!append_format(buf, buf_size, &offset, "{\"id\":%u,\"local_index\":%d,\"local_code\":", output->id, i + 1))
            return offset;
        if (!append_json_string(buf, buf_size, &offset, local_code))
            return offset;
        if (!append_format(buf, buf_size, &offset, ",\"gpio\":%d,\"default_gpio\":%d,\"backend\":", output->gpio, output->default_gpio))
            return offset;
        if (!append_json_string(buf, buf_size, &offset, io_binding_backend_code(output->backend)))
            return offset;
        if (!append_text(buf, buf_size, &offset, ",\"backend_name\":"))
            return offset;
        if (!append_json_string(buf, buf_size, &offset, output->backend_name))
            return offset;
        if (!append_format(buf, buf_size, &offset, ",\"backend_instance\":%d,\"endpoint_index\":%d,\"address\":", output->backend_instance, output->endpoint_index))
            return offset;
        if (!append_json_string(buf, buf_size, &offset, output->address))
            return offset;
        if (!append_text(buf, buf_size, &offset, ",\"backend_address\":"))
            return offset;
        if (!append_json_string(buf, buf_size, &offset, output->address))
            return offset;
        if (!append_text(buf, buf_size, &offset, ",\"display_name_local\":"))
            return offset;
        if (!append_json_string(buf, buf_size, &offset, output->name))
            return offset;
        if (!append_format(buf, buf_size, &offset, ",\"active_low\":%u,\"name\":", profile->active_low ? 1U : 0U))
            return offset;
        if (!append_json_string(buf, buf_size, &offset, output->name))
            return offset;
        if (!append_text(buf, buf_size, &offset, ",\"role\":"))
            return offset;
        if (!append_json_string(buf, buf_size, &offset, output->role))
            return offset;
        if (!append_text(buf, buf_size, &offset, ",\"description\":"))
            return offset;
        if (!append_json_string(buf, buf_size, &offset, output->description))
            return offset;
        if (!append_text(buf, buf_size, &offset, ",\"plc_code\":"))
            return offset;
        if (!append_json_string(buf, buf_size, &offset, plc_out ? plc_out->plc_code : local_code))
            return offset;
        if (!append_text(buf, buf_size, &offset, ",\"plc_name\":"))
            return offset;
        if (!append_json_string(buf, buf_size, &offset, plc_out ? plc_out->default_name : output->name))
            return offset;
        if (!append_format(buf, buf_size, &offset, ",\"plc_available\":%u}", (plc_out && plc_out->available) ? 1U : 0U))
            return offset;

        first = false;
    }

    const node_profile_desc_t *current_tpl = device_profile_get_current();
    if (current_tpl && current_tpl->plc_channels && current_tpl->plc_channels_len > 0)
    {
        if (!append_text(buf, buf_size, &offset, "],\"plc_channels\":["))
            return offset;
        for (size_t p = 0; p < current_tpl->plc_channels_len; p++)
        {
            const plc_channel_desc_t *ch = &current_tpl->plc_channels[p];
            if (p > 0 && !append_text(buf, buf_size, &offset, ","))
                return offset;
            if (!append_format(buf, buf_size, &offset, "{\"plc_code\":"))
                return offset;
            if (!append_json_string(buf, buf_size, &offset, ch->plc_code))
                return offset;
            if (!append_format(buf, buf_size, &offset,
                ",\"class\":%u,\"channel_index\":%u,\"gpio\":%d,\"available\":%u,\"name\":",
                (unsigned)ch->channel_class, (unsigned)ch->channel_index, (int)ch->gpio, ch->available ? 1U : 0U))
                return offset;
            if (!append_json_string(buf, buf_size, &offset, ch->default_name))
                return offset;
            if (!append_text(buf, buf_size, &offset, ",\"role\":"))
                return offset;
            if (!append_json_string(buf, buf_size, &offset, ch->role ? ch->role : ""))
                return offset;
            if (!append_text(buf, buf_size, &offset, "}"))
                return offset;
        }
    }

    if (!append_text(buf, buf_size, &offset, "],\"input_gpio_options\":"))
        return offset;
    if (!append_gpio_option_array(buf, buf_size, &offset, true))
        return offset;
    if (!append_text(buf, buf_size, &offset, ",\"output_gpio_options\":"))
        return offset;
    if (!append_gpio_option_array(buf, buf_size, &offset, false))
        return offset;
    if (!append_text(buf, buf_size, &offset, ",\"available_input_slots\":"))
        return offset;
    if (!append_available_slot_array(buf, buf_size, &offset, true))
        return offset;
    if (!append_text(buf, buf_size, &offset, ",\"available_output_slots\":"))
        return offset;
    if (!append_available_slot_array(buf, buf_size, &offset, false))
        return offset;

    /* Append gpio_inventory */
    {
        device_gpio_inventory_item_t inv[DEVICE_GPIO_INVENTORY_MAX];
        int inv_count = io_binding_export_gpio_inventory(inv, DEVICE_GPIO_INVENTORY_MAX);

        if (!append_text(buf, buf_size, &offset, ",\"gpio_inventory\":["))
            return offset;

        for (int i = 0; i < inv_count; i++)
        {
            if (i > 0 && !append_text(buf, buf_size, &offset, ","))
                return offset;

            if (!append_format(buf, buf_size, &offset,
                               "{\"gpio\":%d,\"capabilities\":[", inv[i].gpio))
                return offset;

            bool has_cap = false;
            if (inv[i].input_capable)
            {
                if (!append_text(buf, buf_size, &offset, "\"input\""))
                    return offset;
                has_cap = true;
            }
            if (inv[i].output_capable)
            {
                if (has_cap && !append_text(buf, buf_size, &offset, ","))
                    return offset;
                if (!append_text(buf, buf_size, &offset, "\"output\""))
                    return offset;
                has_cap = true;
            }
            if (inv[i].analog_capable)
            {
                if (has_cap && !append_text(buf, buf_size, &offset, ","))
                    return offset;
                if (!append_text(buf, buf_size, &offset, "\"analog\""))
                    return offset;
            }

            if (!append_format(buf, buf_size, &offset,
                               "],\"state\":\"%s\",\"reserved_by\":",
                               inv[i].state_str ? inv[i].state_str : "AVAILABLE"))
                return offset;

            if (inv[i].reserved_by)
            {
                if (!append_json_string(buf, buf_size, &offset, inv[i].reserved_by))
                    return offset;
            }
            else
            {
                if (!append_text(buf, buf_size, &offset, "null"))
                    return offset;
            }

            if (inv[i].bound_id > 0)
            {
                if (!append_format(buf, buf_size, &offset, ",\"bound_id\":%u", inv[i].bound_id))
                    return offset;
            }
            else
            {
                if (!append_text(buf, buf_size, &offset, ",\"bound_id\":null"))
                    return offset;
            }

            if (inv[i].plc_channel[0])
            {
                if (!append_format(buf, buf_size, &offset, ",\"plc_channel\":\"%s\"", inv[i].plc_channel))
                    return offset;
            }
            else
            {
                if (!append_text(buf, buf_size, &offset, ",\"plc_channel\":null"))
                    return offset;
            }

            if (!append_text(buf, buf_size, &offset, "}"))
                return offset;
        }

        if (!append_text(buf, buf_size, &offset, "]"))
            return offset;
    }

    if (!append_profile_context_sections(buf, buf_size, &offset, network, input_count, output_count))
        return offset;

    if (!append_format(buf,
                       buf_size,
                       &offset,
                       ",\"channel_identity\":{\"base_model\":\"node-local\",\"installation_mapping\":\"gateway-api\",\"default_input_block\":%u,\"default_output_block\":%u,\"output_global_base\":%u},\"input_slot_capacity\":%d,\"output_slot_capacity\":%d,\"active_input_count\":%d,\"active_output_count\":%d,\"gpio_restart_required\":%u,\"network\":{",
                       INSTALLATION_INPUT_BLOCK_DEFAULT,
                       INSTALLATION_OUTPUT_BLOCK_DEFAULT,
                       INSTALLATION_OUTPUT_BASE_DEFAULT,
                       device_profile_input_count(),
                       device_profile_output_count(),
                       input_count,
                       output_count,
                       io_binding_gpio_restart_required() ? 1U : 0U))
        return offset;

    if (!append_format(buf,
                       buf_size,
                       &offset,
                       "\"wifi_supported\":%u,\"wifi_enabled\":%u,\"ethernet_supported\":%u,\"ethernet_enabled\":%u,\"rs485_supported\":%u,\"rs485_enabled\":%u,\"ethernet_configured\":%u,\"wifi_mode\":%u,\"ethernet_mode\":%u,\"label\":",
                       (network && network->wifi_supported) ? 1U : 0U,
                       (network && network->wifi_enabled) ? 1U : 0U,
                       (network && network->ethernet_supported) ? 1U : 0U,
                       (network && network->ethernet_enabled) ? 1U : 0U,
                       (network && network->rs485_supported) ? 1U : 0U,
                       (network && network->rs485_enabled) ? 1U : 0U,
                       ethernet_configured ? 1U : 0U,
                       (network) ? (uint32_t)network->wifi_mode : 0U,
                       (network) ? (uint32_t)network->ethernet_mode : 0U))
        return offset;

    if (!append_json_string(buf, buf_size, &offset, (network && network->label) ? network->label : ""))
        return offset;

    if (network && network->ethernet_mode == DEVICE_PROFILE_ETH_SPI_W5500 && w5500)
    {
        if (!append_format(buf,
                           buf_size,
                           &offset,
                           ",\"w5500\":{\"spi_host\":%d,\"clock_mhz\":%u,\"phy_addr\":%d,\"mosi_gpio\":%d,\"miso_gpio\":%d,\"sclk_gpio\":%d,\"cs_gpio\":%d,\"int_gpio\":%d,\"reset_gpio\":%d}",
                           w5500->spi_host_id,
                           w5500->clock_mhz,
                           w5500->phy_addr,
                           (int)w5500->mosi_gpio,
                           (int)w5500->miso_gpio,
                           (int)w5500->sclk_gpio,
                           (int)w5500->cs_gpio,
                           (int)w5500->int_gpio,
                           (int)w5500->reset_gpio))
            return offset;
    }

    if (!append_text(buf, buf_size, &offset, "}}"))
        return offset;

    return offset;
}

static size_t build_public_profile_json(char *buf, size_t buf_size)
{
    size_t offset = 0;
    int input_count = io_binding_export_inputs(input_profile_snapshot, IO_BINDING_MAX_INPUTS);
    int output_count = io_binding_export_outputs(output_profile_snapshot, IO_BINDING_MAX_OUTPUTS);
    const device_network_profile_t *network = device_profile_network();
    bool first = true;

    if (!buf || buf_size == 0U)
        return 0;

    buf[0] = '\0';

    if (!append_text(buf, buf_size, &offset, "{\"inputs\":["))
        return 0;

    for (int i = 0; i < input_count; i++)
    {
        const io_binding_input_view_t *binding = &input_profile_snapshot[i];
        const device_input_profile_t *input = device_profile_find_input(binding->id);
        char local_code[INSTALLATION_MAP_LOCAL_CODE_LEN];

        if (!binding || binding->id == 0 || !input)
            continue;

        installation_local_code(true, i + 1, local_code);

        if (!append_text(buf, buf_size, &offset, first ? "" : ","))
            return 0;

        if (!append_format(buf, buf_size, &offset, "{\"id\":%u,\"local_index\":%d,\"local_code\":", binding->id, i + 1))
            return 0;
        if (!append_json_string(buf, buf_size, &offset, local_code))
            return 0;
        if (!append_format(buf, buf_size, &offset, ",\"gpio\":%d,\"default_gpio\":%d,\"backend\":", binding->gpio, binding->default_gpio))
            return 0;
        if (!append_json_string(buf, buf_size, &offset, io_binding_backend_code(binding->backend)))
            return 0;
        if (!append_text(buf, buf_size, &offset, ",\"backend_name\":"))
            return 0;
        if (!append_json_string(buf, buf_size, &offset, binding->backend_name))
            return 0;
        if (!append_format(buf, buf_size, &offset, ",\"backend_instance\":%d,\"endpoint_index\":%d,\"address\":", binding->backend_instance, binding->endpoint_index))
            return 0;
        if (!append_json_string(buf, buf_size, &offset, binding->address))
            return 0;
        if (!append_text(buf, buf_size, &offset, ",\"backend_address\":"))
            return 0;
        if (!append_json_string(buf, buf_size, &offset, binding->address))
            return 0;
        if (!append_text(buf, buf_size, &offset, ",\"display_name_local\":"))
            return 0;
        if (!append_json_string(buf, buf_size, &offset, binding->name))
            return 0;
        if (!append_format(buf, buf_size, &offset, ",\"active_low\":%u,\"debounce\":%u,\"name\":", input->active_low ? 1U : 0U, input->debounce_samples))
            return 0;
        if (!append_json_string(buf, buf_size, &offset, binding->name))
            return 0;
        if (!append_text(buf, buf_size, &offset, ",\"role\":"))
            return 0;
        if (!append_json_string(buf, buf_size, &offset, binding->role))
            return 0;
        const plc_channel_desc_t *plc_in = find_plc_channel_desc(DEVICE_CHANNEL_CLASS_DIGITAL_INPUT, i);

        if (!append_text(buf, buf_size, &offset, ",\"description\":"))
            return 0;
        if (!append_json_string(buf, buf_size, &offset, binding->description))
            return 0;
        if (!append_text(buf, buf_size, &offset, ",\"plc_code\":"))
            return 0;
        if (!append_json_string(buf, buf_size, &offset, plc_in ? plc_in->plc_code : local_code))
            return 0;
        if (!append_text(buf, buf_size, &offset, ",\"plc_name\":"))
            return 0;
        if (!append_json_string(buf, buf_size, &offset, plc_in ? plc_in->default_name : binding->name))
            return 0;
        if (!append_format(buf, buf_size, &offset, ",\"plc_available\":%u}", (plc_in && plc_in->available) ? 1U : 0U))
            return 0;

        first = false;
    }

    if (!append_text(buf, buf_size, &offset, "],\"outputs\":["))
        return 0;
    first = true;

    for (int i = 0; i < output_count; i++)
    {
        const io_binding_output_view_t *output = &output_profile_snapshot[i];
        const device_output_profile_t *profile = device_profile_find_output(output->id);
        const plc_channel_desc_t *plc_out = find_plc_channel_desc(DEVICE_CHANNEL_CLASS_DIGITAL_OUTPUT, i);
        char local_code[INSTALLATION_MAP_LOCAL_CODE_LEN];

        if (!output || output->id == 0 || !profile)
            continue;

        installation_local_code(false, i + 1, local_code);

        if (!append_text(buf, buf_size, &offset, first ? "" : ","))
            return 0;

        if (!append_format(buf, buf_size, &offset, "{\"id\":%u,\"local_index\":%d,\"local_code\":", output->id, i + 1))
            return 0;
        if (!append_json_string(buf, buf_size, &offset, local_code))
            return 0;
        if (!append_format(buf, buf_size, &offset, ",\"gpio\":%d,\"default_gpio\":%d,\"backend\":", output->gpio, output->default_gpio))
            return 0;
        if (!append_json_string(buf, buf_size, &offset, io_binding_backend_code(output->backend)))
            return 0;
        if (!append_text(buf, buf_size, &offset, ",\"backend_name\":"))
            return 0;
        if (!append_json_string(buf, buf_size, &offset, output->backend_name))
            return 0;
        if (!append_format(buf, buf_size, &offset, ",\"backend_instance\":%d,\"endpoint_index\":%d,\"address\":", output->backend_instance, output->endpoint_index))
            return 0;
        if (!append_json_string(buf, buf_size, &offset, output->address))
            return 0;
        if (!append_text(buf, buf_size, &offset, ",\"backend_address\":"))
            return 0;
        if (!append_json_string(buf, buf_size, &offset, output->address))
            return 0;
        if (!append_text(buf, buf_size, &offset, ",\"display_name_local\":"))
            return 0;
        if (!append_json_string(buf, buf_size, &offset, output->name))
            return 0;
        if (!append_format(buf, buf_size, &offset, ",\"active_low\":%u,\"name\":", profile->active_low ? 1U : 0U))
            return 0;
        if (!append_json_string(buf, buf_size, &offset, output->name))
            return 0;
        if (!append_text(buf, buf_size, &offset, ",\"role\":"))
            return 0;
        if (!append_json_string(buf, buf_size, &offset, output->role))
            return 0;
        if (!append_text(buf, buf_size, &offset, ",\"description\":"))
            return 0;
        if (!append_json_string(buf, buf_size, &offset, output->description))
            return 0;
        if (!append_text(buf, buf_size, &offset, ",\"plc_code\":"))
            return 0;
        if (!append_json_string(buf, buf_size, &offset, plc_out ? plc_out->plc_code : local_code))
            return 0;
        if (!append_text(buf, buf_size, &offset, ",\"plc_name\":"))
            return 0;
        if (!append_json_string(buf, buf_size, &offset, plc_out ? plc_out->default_name : output->name))
            return 0;
        if (!append_format(buf, buf_size, &offset, ",\"plc_available\":%u}", (plc_out && plc_out->available) ? 1U : 0U))
            return 0;

        first = false;
    }

    const node_profile_desc_t *current_tpl = device_profile_get_current();
    if (current_tpl && current_tpl->plc_channels && current_tpl->plc_channels_len > 0)
    {
        if (!append_text(buf, buf_size, &offset, "],\"plc_channels\":["))
            return 0;
        for (size_t p = 0; p < current_tpl->plc_channels_len; p++)
        {
            const plc_channel_desc_t *ch = &current_tpl->plc_channels[p];
            if (p > 0 && !append_text(buf, buf_size, &offset, ","))
                return 0;
            if (!append_format(buf, buf_size, &offset, "{\"plc_code\":"))
                return 0;
            if (!append_json_string(buf, buf_size, &offset, ch->plc_code))
                return 0;
            if (!append_format(buf, buf_size, &offset,
                ",\"class\":%u,\"channel_index\":%u,\"gpio\":%d,\"available\":%u,\"name\":",
                (unsigned)ch->channel_class, (unsigned)ch->channel_index, (int)ch->gpio, ch->available ? 1U : 0U))
                return 0;
            if (!append_json_string(buf, buf_size, &offset, ch->default_name))
                return 0;
            if (!append_text(buf, buf_size, &offset, ",\"role\":"))
                return 0;
            if (!append_json_string(buf, buf_size, &offset, ch->role ? ch->role : ""))
                return 0;
            if (!append_text(buf, buf_size, &offset, "}"))
                return 0;
        }
    }

    if (!append_text(buf, buf_size, &offset, "]"))
        return 0;

    /* Append gpio_inventory */
    {
        device_gpio_inventory_item_t inv[DEVICE_GPIO_INVENTORY_MAX];
        int inv_count = io_binding_export_gpio_inventory(inv, DEVICE_GPIO_INVENTORY_MAX);

        if (!append_text(buf, buf_size, &offset, ",\"gpio_inventory\":["))
            return 0;

        for (int i = 0; i < inv_count; i++)
        {
            if (i > 0 && !append_text(buf, buf_size, &offset, ","))
                return 0;

            if (!append_format(buf, buf_size, &offset,
                               "{\"gpio\":%d,\"capabilities\":[", inv[i].gpio))
                return 0;

            bool has_cap = false;
            if (inv[i].input_capable)
            {
                if (!append_text(buf, buf_size, &offset, "\"input\""))
                    return 0;
                has_cap = true;
            }
            if (inv[i].output_capable)
            {
                if (has_cap && !append_text(buf, buf_size, &offset, ","))
                    return 0;
                if (!append_text(buf, buf_size, &offset, "\"output\""))
                    return 0;
                has_cap = true;
            }
            if (inv[i].analog_capable)
            {
                if (has_cap && !append_text(buf, buf_size, &offset, ","))
                    return 0;
                if (!append_text(buf, buf_size, &offset, "\"analog\""))
                    return 0;
            }

            if (!append_format(buf, buf_size, &offset,
                               "],\"state\":\"%s\",\"reserved_by\":",
                               inv[i].state_str ? inv[i].state_str : "AVAILABLE"))
                return 0;

            if (inv[i].reserved_by)
            {
                if (!append_json_string(buf, buf_size, &offset, inv[i].reserved_by))
                    return 0;
            }
            else
            {
                if (!append_text(buf, buf_size, &offset, "null"))
                    return 0;
            }

            if (inv[i].bound_id > 0)
            {
                if (!append_format(buf, buf_size, &offset, ",\"bound_id\":%u", inv[i].bound_id))
                    return 0;
            }
            else
            {
                if (!append_text(buf, buf_size, &offset, ",\"bound_id\":null"))
                    return 0;
            }

            if (inv[i].plc_channel[0])
            {
                if (!append_format(buf, buf_size, &offset, ",\"plc_channel\":\"%s\"", inv[i].plc_channel))
                    return 0;
            }
            else
            {
                if (!append_text(buf, buf_size, &offset, ",\"plc_channel\":null"))
                    return 0;
            }

            if (!append_text(buf, buf_size, &offset, "}"))
                return 0;
        }

        if (!append_text(buf, buf_size, &offset, "]"))
            return 0;
    }

    if (!append_format(buf,
                       buf_size,
                       &offset,
                       ",\"channel_identity\":{\"base_model\":\"node-local\",\"installation_mapping\":\"gateway-api\",\"default_input_block\":%u,\"default_output_block\":%u,\"output_global_base\":%u}",
                       INSTALLATION_INPUT_BLOCK_DEFAULT,
                       INSTALLATION_OUTPUT_BLOCK_DEFAULT,
                       INSTALLATION_OUTPUT_BASE_DEFAULT))
        return 0;

    if (!append_profile_context_sections(buf, buf_size, &offset, network, input_count, output_count))
        return 0;

    esp_netif_t *sta_netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    esp_netif_ip_info_t ip_info = {0};
    bool is_connected = false;
    if (sta_netif) {
        esp_netif_get_ip_info(sta_netif, &ip_info);
        if (ip_info.ip.addr != 0) is_connected = true;
    }
    wifi_mode_t mode = WIFI_MODE_NULL;
    esp_wifi_get_mode(&mode);
    const char *mode_str = "NONE";
    if (mode == WIFI_MODE_STA) mode_str = "STA";
    else if (mode == WIFI_MODE_AP) mode_str = "AP";
    else if (mode == WIFI_MODE_APSTA) mode_str = "AP+STA";

    char ip_str[16] = {0}, nm_str[16] = {0}, gw_str[16] = {0};
    if (is_connected) {
        esp_ip4addr_ntoa(&ip_info.ip, ip_str, sizeof(ip_str));
        esp_ip4addr_ntoa(&ip_info.netmask, nm_str, sizeof(nm_str));
        esp_ip4addr_ntoa(&ip_info.gw, gw_str, sizeof(gw_str));
    }

    if (!append_format(buf,
                       buf_size,
                       &offset,
                       ",\"input_slot_capacity\":%d,\"output_slot_capacity\":%d,\"active_input_count\":%d,\"active_output_count\":%d,\"network\":{\"wifi_supported\":%u,\"wifi_enabled\":%u,\"ethernet_supported\":%u,\"ethernet_enabled\":%u,\"rs485_supported\":%u,\"rs485_enabled\":%u,\"wifi_connected\":%u,\"wifi_mode\":\"%s\",\"ipv4\":\"%s\",\"netmask\":\"%s\",\"gateway\":\"%s\",\"hostname\":\"%s\"}",
                       device_profile_input_count(),
                       device_profile_output_count(),
                       input_count,
                       output_count,
                       (network && network->wifi_supported) ? 1U : 0U,
                       (network && network->wifi_enabled) ? 1U : 0U,
                       (network && network->ethernet_supported) ? 1U : 0U,
                       (network && network->ethernet_enabled) ? 1U : 0U,
                       (network && network->rs485_supported) ? 1U : 0U,
                       (network && network->rs485_enabled) ? 1U : 0U,
                       is_connected ? 1U : 0U,
                       mode_str,
                       ip_str,
                       nm_str,
                       gw_str,
                       "endap.local"))
        return 0;

    if (!append_text(buf, buf_size, &offset, "}"))
        return 0;

    return offset;
}

static esp_err_t public_profile_handler(httpd_req_t *req)
{
    char *json_buf = json_buffer_malloc(PUBLIC_PROFILE_JSON_BUFFER_SIZE);
    if (!json_buf) { json_buffer_free(NULL); httpd_resp_send_500(req); return ESP_OK; }
    size_t len = build_public_profile_json(json_buf, PUBLIC_PROFILE_JSON_BUFFER_SIZE);

    http_set_public_json_headers(req);

    if (len == 0U)
        httpd_resp_send_500(req);
    else
        httpd_resp_send(req, json_buf, len);

    json_buffer_free(json_buf);

    return ESP_OK;
}

/* ============================================================
   WIFI SAVE
============================================================ */

static esp_err_t wifi_confirm_handler(httpd_req_t *req)
{
    if (!http_auth_require_cap(req, AUTH_CAP_TRANSPORT_WRITE))
        return ESP_OK;

    http_set_private_json_headers(req);

    wifi_manager_confirm_onboarding();

    httpd_resp_sendstr(req, "{\"ok\":true}");
    return ESP_OK;
}

static esp_err_t wifi_save_handler(httpd_req_t *req)
{
    const device_network_profile_t *network = device_profile_network();
    char query[128];
    char ssid[33] = {0};
    char pass[65] = {0};
    esp_err_t err;

    if (!http_auth_require_cap(req, AUTH_CAP_TRANSPORT_WRITE))
    return ESP_OK;

    http_set_private_text_headers(req);

    if (!network_transport_wifi_enabled(network))
    {
        httpd_resp_set_status(req, "409 Conflict");
        httpd_resp_sendstr(req, "WIFI_DISABLED");
    return ESP_OK;
    }

    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK)
    {
        httpd_query_key_value(query, "ssid", ssid, sizeof(ssid));
        httpd_query_key_value(query, "pass", pass, sizeof(pass));
    }

    err = wifi_manager_save(ssid, pass);

    if (err == ESP_ERR_INVALID_ARG)
    {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_sendstr(req, "INVALID_WIFI_CONFIG");
    return ESP_OK;
    }

    if (err != ESP_OK)
    {
        httpd_resp_set_status(req, "500 Internal Server Error");
        httpd_resp_sendstr(req, "WIFI_SAVE_FAILED");
    return ESP_OK;
    }

    auth_audit_log("wifi_credentials_updated", ssid[0] ? ssid : "ssid-empty");
    httpd_resp_sendstr(req, "CONNECTING...");
    return ESP_OK;
}

static size_t build_wifi_status_json(char *buf, size_t buf_size)
{
    size_t offset = 0;
    wifi_manager_status_t wifi = {0};
    const device_network_profile_t *network = device_profile_network();
    bool wifi_enabled = network_transport_wifi_enabled(network);

    if (!buf || buf_size == 0U)
        return 0;

    buf[0] = '\0';

    if (wifi_enabled)
        wifi_manager_get_status(&wifi);

    if (!append_format(buf,
                       buf_size,
                       &offset,
                       "{\"enabled\":%u,\"initialized\":%u,\"credentials_saved\":%u,\"sta_requested\":%u,"
                       "\"sta_connected\":%u,\"ap_fallback_active\":%u,\"retry_count\":%u,"
                       "\"retry_limit\":%u,\"last_disconnect_reason\":%u,\"state_code\":%u,\"state\":",
                       wifi_enabled ? 1U : 0U,
                       wifi.initialized ? 1U : 0U,
                       wifi.credentials_saved ? 1U : 0U,
                       wifi.sta_requested ? 1U : 0U,
                       wifi.sta_connected ? 1U : 0U,
                       wifi.ap_fallback_active ? 1U : 0U,
                       wifi.retry_count,
                       wifi.retry_limit,
                       wifi.last_disconnect_reason,
                       (unsigned int)wifi.state))
        return 0;

    if (!append_json_string(buf,
                            buf_size,
                            &offset,
                            wifi_enabled ? wifi_manager_state_name(wifi.state) : "disabled"))
        return 0;

    if (!append_text(buf, buf_size, &offset, ",\"ssid\":"))
        return 0;

    if (!append_json_string(buf, buf_size, &offset, wifi.ssid))
        return 0;

    if (!append_text(buf, buf_size, &offset, "}"))
        return 0;

    return offset;
}

static size_t build_wifi_scan_json(char *buf, size_t buf_size)
{
    size_t offset = 0;
    wifi_manager_scan_result_t results[16];
    size_t count = 0;
    esp_err_t err;
    const device_network_profile_t *network = device_profile_network();

    if (!buf || buf_size == 0U)
        return 0;

    buf[0] = '\0';

    if (!network_transport_wifi_enabled(network))
    {
        if (!append_text(buf, buf_size, &offset,
                         "{\"ok\":false,\"count\":0,\"error\":\"WIFI_DISABLED\",\"networks\":[]}"))
        {
            return 0;
        }

        return offset;
    }

    err = wifi_manager_scan_networks(results, (sizeof(results) / sizeof(results[0])), &count);

    if (!append_text(buf, buf_size, &offset, "{\"ok\":"))
        return 0;

    if (!append_text(buf, buf_size, &offset, (err == ESP_OK) ? "true" : "false"))
        return 0;

    if (!append_format(buf, buf_size, &offset, ",\"count\":%u,\"error\":", (unsigned)count))
        return 0;

    if (!append_json_string(buf, buf_size, &offset, (err == ESP_OK) ? "" : esp_err_to_name(err)))
        return 0;

    if (!append_text(buf, buf_size, &offset, ",\"networks\":["))
        return 0;

    for (size_t i = 0; i < count; i++)
    {
        if (!append_text(buf, buf_size, &offset, (i == 0U) ? "" : ","))
            return 0;

        if (!append_text(buf, buf_size, &offset, "{\"ssid\":"))
            return 0;

        if (!append_json_string(buf, buf_size, &offset, results[i].ssid))
            return 0;

        if (!append_format(buf,
                           buf_size,
                           &offset,
                           ",\"rssi\":%d,\"authmode\":%u,\"auth\":",
                           (int)results[i].rssi,
                           (unsigned)results[i].authmode))
            return 0;

        if (!append_json_string(buf, buf_size, &offset, wifi_authmode_name(results[i].authmode)))
            return 0;

        if (!append_text(buf, buf_size, &offset, "}"))
            return 0;
    }

    if (!append_text(buf, buf_size, &offset, "]}"))
        return 0;

    return offset;
}

static esp_err_t wifi_scan_handler(httpd_req_t *req)
{
    if (!http_auth_require_cap(req, AUTH_CAP_TRANSPORT_WRITE))
        return ESP_OK;

    char *json_buf = json_buffer_malloc(WIFI_SCAN_JSON_BUFFER_SIZE);
    if (!json_buf) { json_buffer_free(NULL); httpd_resp_send_500(req); return ESP_OK; }

    size_t len = build_wifi_scan_json(json_buf, WIFI_SCAN_JSON_BUFFER_SIZE);

    http_set_public_json_headers(req);

    if (len == 0U)
        httpd_resp_send_500(req);
    else
        httpd_resp_send(req, json_buf, len);

    json_buffer_free(json_buf);

    return ESP_OK;
}

static query_value_status_t query_get_value(httpd_req_t *req,
                                            const char *key,
                                            char *out_value,
                                            size_t out_size)
{
    char query[HTTP_QUERY_BUFFER_SIZE];
    esp_err_t err;

    if (!req || !key || !out_value || out_size == 0U)
        return QUERY_VALUE_INVALID;

    out_value[0] = '\0';
    err = httpd_req_get_url_query_str(req, query, sizeof(query));
    if (err == ESP_ERR_NOT_FOUND)
        return QUERY_VALUE_MISSING;
    if (err != ESP_OK)
        return QUERY_VALUE_INVALID;

    err = httpd_query_key_value(query, key, out_value, out_size);
    if (err == ESP_ERR_NOT_FOUND)
        return QUERY_VALUE_MISSING;
    if (err != ESP_OK)
        return QUERY_VALUE_INVALID;

    return QUERY_VALUE_OK;
}

static bool query_parse_int(const char *text, int min_value, int max_value, int *out_value)
{
    long parsed;
    char *endptr = NULL;

    if (!text || !out_value || text[0] == '\0')
        return false;

    errno = 0;
    parsed = strtol(text, &endptr, 10);
    if (errno != 0 || !endptr || *endptr != '\0')
        return false;

    if (parsed < (long)min_value || parsed > (long)max_value)
        return false;

    *out_value = (int)parsed;
    return true;
}

static bool query_parse_u32(const char *text, uint32_t *out_value)
{
    unsigned long parsed;
    char *endptr = NULL;

    if (!text || !out_value || text[0] == '\0' || text[0] == '-')
        return false;

    errno = 0;
    parsed = strtoul(text, &endptr, 10);
    if (errno != 0 || !endptr || *endptr != '\0' || parsed > UINT32_MAX)
        return false;

    *out_value = (uint32_t)parsed;
    return true;
}

static bool query_get_int(httpd_req_t *req,
                          const char *key,
                          int min_value,
                          int max_value,
                          int *out_value)
{
    char param[32];

    if (query_get_value(req, key, param, sizeof(param)) != QUERY_VALUE_OK)
        return false;

    return query_parse_int(param, min_value, max_value, out_value);
}

static bool query_get_optional_int(httpd_req_t *req,
                                   const char *key,
                                   int min_value,
                                   int max_value,
                                   int *out_value,
                                   bool *out_found)
{
    char param[32];
    query_value_status_t status = query_get_value(req, key, param, sizeof(param));

    if (out_found)
        *out_found = false;

    if (status == QUERY_VALUE_MISSING)
        return true;

    if (status != QUERY_VALUE_OK || !query_parse_int(param, min_value, max_value, out_value))
        return false;

    if (out_found)
        *out_found = true;

    return true;
}

static bool query_get_u32(httpd_req_t *req, const char *key, uint32_t *out_value)
{
    char param[32];

    if (query_get_value(req, key, param, sizeof(param)) != QUERY_VALUE_OK)
        return false;

    return query_parse_u32(param, out_value);
}

static bool query_get_optional_u32(httpd_req_t *req,
                                   const char *key,
                                   uint32_t *out_value,
                                   bool *out_found)
{
    char param[32];
    query_value_status_t status = query_get_value(req, key, param, sizeof(param));

    if (out_found)
        *out_found = false;

    if (status == QUERY_VALUE_MISSING)
        return true;

    if (status != QUERY_VALUE_OK || !query_parse_u32(param, out_value))
        return false;

    if (out_found)
        *out_found = true;

    return true;
}

static bool query_get_str(httpd_req_t *req, const char *key, char *out_value, size_t out_size)
{
    return query_get_value(req, key, out_value, out_size) == QUERY_VALUE_OK;
}

static bool query_get_optional_str(httpd_req_t *req,
                                   const char *key,
                                   char *out_value,
                                   size_t out_size,
                                   bool *out_found)
{
    query_value_status_t status = query_get_value(req, key, out_value, out_size);

    if (out_found)
        *out_found = false;

    if (status == QUERY_VALUE_MISSING)
        return true;

    if (status != QUERY_VALUE_OK)
        return false;

    if (out_found)
        *out_found = true;

    return true;
}

/* ============================================================
   WEBSOCKET
============================================================ */

static esp_err_t ws_handler(httpd_req_t *req)
{
    if (req->method == HTTP_GET)
    {
        int fd = httpd_req_to_sockfd(req);
        bool already_registered = false;

        if (!http_auth_require(req))
            return ESP_OK;

        portENTER_CRITICAL(&ws_lock);

        for (int i = 0; i < ws_count; i++)
        {
            if (ws_clients[i] == fd)
            {
                already_registered = true;
                break;
            }
        }

        if (!already_registered && ws_count < HTTP_WS_MAX_CLIENTS)
            ws_clients[ws_count++] = fd;

        portEXIT_CRITICAL(&ws_lock);

        http_server_notify_state_change();

        return ESP_OK;
    }

    return ESP_OK;
}

/* ============================================================
   WS BROADCAST
============================================================ */

void http_ws_broadcast_state(void)
{
    int clients[HTTP_WS_MAX_CLIENTS];
    int client_count = 0;
    int alive_clients[HTTP_WS_MAX_CLIENTS];
    int alive_count = 0;

    if (!server) return;

    portENTER_CRITICAL(&ws_lock);
    client_count = ws_count;
    memcpy(clients, ws_clients, sizeof(int) * (size_t)client_count);
    portEXIT_CRITICAL(&ws_lock);

    char *json_buf = json_buffer_malloc(STATUS_JSON_BUFFER_SIZE);
    if (!json_buf) { json_buffer_free(NULL); return; }

    size_t msg_len = build_status_json(json_buf, STATUS_JSON_BUFFER_SIZE);

    httpd_ws_frame_t frame = {
        .type = HTTPD_WS_TYPE_TEXT,
        .payload = (uint8_t*)json_buf,
        .len = msg_len
    };

    for (int i = 0; i < client_count; i++)
    {
        if (httpd_ws_send_frame_async(server, clients[i], &frame) == ESP_OK)
            alive_clients[alive_count++] = clients[i];
    }

    portENTER_CRITICAL(&ws_lock);
    ws_count = alive_count;
    memcpy(ws_clients, alive_clients, sizeof(int) * (size_t)alive_count);
    portEXIT_CRITICAL(&ws_lock);

    json_buffer_free(json_buf);
}

void http_server_notify_state_change(void)
{
    portENTER_CRITICAL(&ws_lock);
    ws_broadcast_pending = true;
    portEXIT_CRITICAL(&ws_lock);
}

void http_server_process(void)
{
    bool has_clients;
    bool pending;
    bool periodic_due = false;
    bool should_broadcast = false;
    uint64_t now_us;

    portENTER_CRITICAL(&ws_lock);
    has_clients = (ws_count > 0);
    pending = ws_broadcast_pending;
    portEXIT_CRITICAL(&ws_lock);

    if (!has_clients && !pending)
        return;

    now_us = (uint64_t)esp_timer_get_time();
    
    portENTER_CRITICAL(&ws_lock);
    periodic_due = has_clients && ((now_us - ws_last_periodic_us) >= WS_PERIODIC_INTERVAL_US);

    if (has_clients && (ws_broadcast_pending || periodic_due))
    {
        ws_broadcast_pending = false;
        ws_last_periodic_us = now_us;
        should_broadcast = true;
    }

    portEXIT_CRITICAL(&ws_lock);

    if (!should_broadcast)
        return;

    http_ws_broadcast_state();
}

/* ============================================================
   API IO
============================================================ */

static esp_err_t set_handler(httpd_req_t *req)
{
    int id = 0, value = 0;
    uint32_t target_node = 0U;
    bool target_found = false;
    bool explicit_remote_target = false;
    cluster_metrics_t metrics = {0};
    char audit_detail[96];
    char resource_id_buf[64] = {0};
    bool using_resource_id = false;
    const char *resource_name = "Unknown";
    int gpio_num = -1;

    if (!http_auth_require_cap(req, AUTH_CAP_MANUAL_IO))
        return ESP_OK;

    http_set_private_text_headers(req);

    if (query_get_str(req, "resource_id", resource_id_buf, sizeof(resource_id_buf)) && resource_id_buf[0] != '\0')
    {
        using_resource_id = true;
        installation_map_entry_t *entry = installation_map_find_by_resource_id(resource_id_buf);
        if (!entry || entry->kind != 1) // 1 == digital_output
        {
            ESP_LOGE("MANUAL_IO", "FAIL: RESOURCE_NOT_FOUND (%s)", resource_id_buf);
            httpd_resp_set_status(req, "404 Not Found");
            httpd_resp_sendstr(req, "RESOURCE_NOT_FOUND");
            return ESP_OK;
        }
        
        resource_name = entry->alias[0] != '\0' ? entry->alias : resource_id_buf;
        id = entry->channel_id;
        target_node = entry->node_id;
        target_found = true;
        if (!query_get_int(req, "value", 0, 1, &value)) {
            httpd_resp_set_status(req, "400 Bad Request");
            httpd_resp_sendstr(req, "BAD_REQUEST");
            return ESP_OK;
        }
    }
    else if (!query_get_int(req, "id", 0, UINT16_MAX, &id) ||
        !query_get_int(req, "value", 0, 1, &value) ||
        !query_get_optional_u32(req, "target_node", &target_node, &target_found))
    {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_sendstr(req, "BAD_REQUEST");
        return ESP_OK;
    }

    if (!device_profile_is_valid_output((uint16_t)id))
    {
        ESP_LOGE("MANUAL_IO", "FAIL: BINDING_NOT_FOUND (Output %d)", id);
        httpd_resp_set_status(req, "404 Not Found");
        httpd_resp_sendstr(req, "NOT_FOUND");
        return ESP_OK;
    }

    metrics = cluster_get_metrics();
    explicit_remote_target = target_found &&
                             target_node != 0U &&
                             metrics.self_node != 0U &&
                             target_node != metrics.self_node;

    if (!explicit_remote_target && (!target_found || target_node == 0U))
    {
        const device_output_profile_t *op = device_profile_find_output((uint16_t)id);
        if (op) {
            gpio_num = op->gpio;
            if (op->gpio == (gpio_num_t)-1) {
                ESP_LOGE("MANUAL_IO", "FAIL: GPIO_RESERVED (Output %d has no GPIO)", id);
            }
        }
    }

    ESP_LOGI("MANUAL_IO", "\n"
             "MANUAL_IO:\n"
             "Resource:\n%s\n"
             "Resource ID:\n%s\n"
             "Channel:\nOutput %d\n"
             "GPIO:\n%d\n"
             "Command:\n%s",
             resource_name,
             using_resource_id ? resource_id_buf : "-",
             id,
             gpio_num,
             value ? "ON" : "OFF");

    if (explicit_remote_target ||
        !cluster_io_is_local((uint16_t)id))
    {
        protocol_msg_t msg = {0};
        uint32_t owner = 0U;

        if (metrics.self_node == 0U)
        {
            ESP_LOGE("MANUAL_IO", "FAIL: SELF_NODE_UNAVAILABLE");
            httpd_resp_set_status(req, "409 Conflict");
            httpd_resp_sendstr(req, "SELF_NODE_UNAVAILABLE");
            return ESP_OK;
        }

        owner = (target_found && target_node != 0U) ? target_node : cluster_io_get_owner((uint16_t)id);

        if (owner == 0U || owner == metrics.self_node)
        {
            ESP_LOGE("MANUAL_IO", "FAIL: REMOTE_OWNER_UNRESOLVED");
            httpd_resp_set_status(req, "409 Conflict");
            httpd_resp_sendstr(req, "REMOTE_OWNER_UNRESOLVED");
            return ESP_OK;
        }

        if (!node_registry_is_operational(owner))
        {
            ESP_LOGE("MANUAL_IO", "FAIL: REMOTE_OWNER_OFFLINE");
            httpd_resp_set_status(req, "409 Conflict");
            httpd_resp_sendstr(req, "REMOTE_OWNER_OFFLINE");
            return ESP_OK;
        }

        if (!cluster_transport_is_ready())
        {
            ESP_LOGE("MANUAL_IO", "FAIL: TRANSPORT_NOT_READY");
            httpd_resp_set_status(req, "503 Service Unavailable");
            httpd_resp_sendstr(req, "TRANSPORT_NOT_READY");
            return ESP_OK;
        }

        msg.type = PROTOCOL_MSG_OUTPUT_COMMAND;
        msg.data.output_command.target_node = owner;
        msg.data.output_command.requester_node = metrics.self_node;
        msg.data.output_command.output_id = (uint16_t)id;
        msg.data.output_command.value = value;

        if (!cluster_transport_broadcast_frame((const uint8_t *)&msg, sizeof(msg)))
        {
            ESP_LOGE("MANUAL_IO", "FAIL: DISPATCH_FAILED");
            httpd_resp_set_status(req, "503 Service Unavailable");
            httpd_resp_sendstr(req, "DISPATCH_FAILED");
            return ESP_OK;
        }

        snprintf(audit_detail,
                 sizeof(audit_detail),
                 "output=%d value=%d target=%" PRIu32 "%s",
                 id,
                 value,
                 owner,
                 target_found ? " explicit" : "");
        auth_audit_log("manual_output_command_remote", audit_detail);
        http_server_notify_state_change();
        ESP_LOGI("MANUAL_IO", "Result:\nSUCCESS (REMOTE DISPATCHED)");
        httpd_resp_set_status(req, "202 Accepted");
        httpd_resp_sendstr(req, "DISPATCHED_REMOTE");
        return ESP_OK;
    }

    int32_t effective_value = value;
    const char *failsafe_reason = NULL;    if (!failsafe_guard_command((uint16_t)id,
                                value,
                                FAILSAFE_COMMAND_MANUAL,
                                &effective_value,
                                &failsafe_reason))
    {
        snprintf(audit_detail,
                 sizeof(audit_detail),
                 "output=%d value=%d blocked reason=%s",
                 id,
                 value,
                 failsafe_reason ? failsafe_reason : "unknown");
        auth_audit_log("manual_output_blocked_failsafe", audit_detail);
        ESP_LOGE("MANUAL_IO", "FAIL: RUNTIME_REJECTED (FAILSAFE_ACTIVE)");
        httpd_resp_set_status(req, "409 Conflict");
        httpd_resp_sendstr(req, "FAILSAFE_ACTIVE");
        return ESP_OK;
    }

    if (!io_command_push(id, effective_value))
    {
        ESP_LOGE("MANUAL_IO", "FAIL: RUNTIME_REJECTED (BUSY)");
        httpd_resp_set_status(req, "503 Service Unavailable");
        httpd_resp_sendstr(req, "BUSY");
        return ESP_OK;
    }

    snprintf(audit_detail, sizeof(audit_detail), "output=%d value=%d", id, (int)effective_value);
    auth_audit_log("manual_output_command", audit_detail);
    http_server_notify_state_change();
    ESP_LOGI("MANUAL_IO", "Result:\nSUCCESS");
    httpd_resp_sendstr(req, "OK");
    return ESP_OK;
}

static esp_err_t status_handler(httpd_req_t *req)
{
    size_t len;

    if (!http_auth_require_cap(req, AUTH_CAP_DASHBOARD_READ))
        return ESP_OK;

    char *json_buf = json_buffer_malloc(STATUS_JSON_BUFFER_SIZE);
    if (!json_buf) { json_buffer_free(NULL); httpd_resp_send_500(req); return ESP_OK; }

    http_set_private_json_headers(req);
    len = build_status_json(json_buf, STATUS_JSON_BUFFER_SIZE);

    if (len == 0U)
        httpd_resp_send(req, "{}", 2);
    else
        httpd_resp_send(req, json_buf, len);

    json_buffer_free(json_buf);

    return ESP_OK;
}

#ifdef CONFIG_ENDAP_ENABLE_CHAOS_TESTING
extern volatile bool g_chaos_inject_delay;
static esp_err_t chaos_inject_delay_handler(httpd_req_t *req)
{
    if (!http_auth_require_cap_json(req, AUTH_CAP_SECURITY_ADMIN, false))
        return ESP_OK;

    g_chaos_inject_delay = true;
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"ok\":true,\"status\":\"chaos_armed\"}");
    return ESP_OK;
}

static esp_err_t chaos_crash_handler(httpd_req_t *req)
{
    if (!http_auth_require_cap_json(req, AUTH_CAP_SECURITY_ADMIN, false))
        return ESP_OK;

    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"ok\":true,\"status\":\"crashing_now\"}");
    
    ESP_LOGE(TAG, "[CHAOS] Provocando PANIC via Null Pointer Dereference (Hard Fault)!");
    vTaskDelay(pdMS_TO_TICKS(100)); // dá tempo pra enviar a resposta HTTP
    
    volatile int *trap = NULL;
    *trap = 0xDEADBEEF; // 💥 BOOM
    
    return ESP_OK;
}
#endif // CONFIG_ENDAP_ENABLE_CHAOS_TESTING

static esp_err_t public_status_handler(httpd_req_t *req)
{
    size_t len;

    char *json_buf = json_buffer_malloc(STATUS_JSON_BUFFER_SIZE);
    if (!json_buf) { json_buffer_free(NULL); httpd_resp_send_500(req); return ESP_OK; }

    http_set_public_json_headers(req);
    len = build_public_status_json(json_buf, STATUS_JSON_BUFFER_SIZE);

    if (len == 0U)
        httpd_resp_send(req, "{}", 2);
    else
        httpd_resp_send(req, json_buf, len);

    json_buffer_free(json_buf);

    return ESP_OK;
}

static esp_err_t failsafe_json_error(httpd_req_t *req,
                                     const char *status,
                                     const char *error)
{
    char json[96];

    http_set_private_json_headers(req);
    if (status)
        httpd_resp_set_status(req, status);

    snprintf(json, sizeof(json), "{\"ok\":false,\"error\":\"%s\"}", error ? error : "error");
    httpd_resp_sendstr(req, json);
    return ESP_OK;
}

static bool failsafe_query_get_value(httpd_req_t *req,
                                      const char *key,
                                      char *out,
                                      size_t out_size)
{
    char query[HTTP_QUERY_BUFFER_SIZE];

    if (!req || !key || !out || out_size == 0U)
        return false;

    out[0] = '\0';

    if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK)
        return false;

    return httpd_query_key_value(query, key, out, out_size) == ESP_OK;
}

static bool failsafe_body_or_query_value(httpd_req_t *req,
                                           const char *body,
                                           const char *key,
                                           char *out,
                                           size_t out_size)
{
    if (!out || out_size == 0U)
        return false;

    out[0] = '\0';

    if (body && body[0] != '\0' &&
        http_body_get_value(body, key, out, out_size))
    {
        return true;
    }

    return failsafe_query_get_value(req, key, out, out_size);
}

static bool failsafe_resolve_output_id_from_text(const char *text, uint16_t *out_output_id)
{
    const char *p = text;
    int parsed = 0;

    if (!text || !out_output_id)
        return false;

    while (*p != '\0' && !isdigit((unsigned char)*p))
        p++;

    if (*p == '\0' ||
        !query_parse_int(p, 0, UINT16_MAX, &parsed))
    {
        return false;
    }

    if (device_profile_is_valid_output((uint16_t)parsed))
    {
        *out_output_id = (uint16_t)parsed;
        return true;
    }

    /*
     * Compatibilidade:
     * - OUT1..OUT16 / 1..16: código visual/local da dashboard.
     * - 0..15: slot zero-based usado por protótipos antigos.
     * - 100..115: id oficial do io_map.
     */
    if (parsed >= 1 && parsed <= (int)ENDAP_MAX_OUTPUT_SLOTS)
    {
        uint16_t candidate = ENDAP_OUTPUT_ID((uint16_t)(parsed - 1));
        if (device_profile_is_valid_output(candidate))
        {
            *out_output_id = candidate;
            return true;
        }
    }

    if (parsed >= 0 && parsed < (int)ENDAP_MAX_OUTPUT_SLOTS)
    {
        uint16_t candidate = ENDAP_OUTPUT_ID((uint16_t)parsed);
        if (device_profile_is_valid_output(candidate))
        {
            *out_output_id = candidate;
            return true;
        }
    }

    return false;
}

static bool failsafe_request_output_id(httpd_req_t *req,
                                       const char *body,
                                       uint16_t *out_output_id)
{
    const char *keys[] = {"output_id", "id", "channel_id", "output", "global_code", "local_code"};
    char text[32];

    for (size_t i = 0U; i < sizeof(keys) / sizeof(keys[0]); i++)
    {
        if (!failsafe_body_or_query_value(req, body, keys[i], text, sizeof(text)))
            continue;

        if (failsafe_resolve_output_id_from_text(text, out_output_id))
            return true;
    }

    return false;
}

static esp_err_t failsafe_handler(httpd_req_t *req)
{
    char *json;
    size_t len;

    if (!http_auth_require_cap(req, AUTH_CAP_DASHBOARD_READ))
        return ESP_OK;

    http_set_private_json_headers(req);
    json = (char *)json_buffer_malloc(INSTALLATION_MAP_JSON_BUFFER_SIZE);
    if (!json) { json_buffer_free(NULL); return failsafe_json_error(req, "500 Internal Server Error", "no_memory"); }

    len = build_failsafe_json(json, INSTALLATION_MAP_JSON_BUFFER_SIZE);

    if (len == 0U)
        httpd_resp_send(req, "{\"ok\":true,\"count\":0,\"outputs\":[]}", HTTPD_RESP_USE_STRLEN);
    else
        httpd_resp_send(req, json, len);

    json_buffer_free(json);
    return ESP_OK;
}

static esp_err_t failsafe_save_handler(httpd_req_t *req)
{
    char body[HTTP_BODY_BUFFER_SIZE] = {0};
    char enabled_text[16] = {0};
    char boot_text[32] = {0};
    char comm_loss_text[32] = {0};
    char runtime_fault_text[32] = {0};
    char safe_value_text[16] = {0};
    char recovery_text[24] = {0};
    char manual_text[16] = {0};
    char audit_detail[160];
    failsafe_output_status_t current = {0};
    failsafe_action_t boot_action;
    failsafe_action_t comm_loss_action;
    failsafe_action_t runtime_fault_action;
    failsafe_recovery_t recovery_mode;
    uint16_t output_id = 0U;
    int enabled = 1;
    int safe_value = 0;
    int manual_value = 0;

    if (!http_auth_require_cap_json(req, AUTH_CAP_FAILSAFE_WRITE, false))
    return ESP_OK;

    http_set_private_json_headers(req);

    if (!http_read_request_body(req, body, sizeof(body)) && req->content_len > 0)
        return failsafe_json_error(req, "400 Bad Request", "bad_request_body");

    if (!failsafe_request_output_id(req, body, &output_id))
        return failsafe_json_error(req, "400 Bad Request", "invalid_or_missing_output_id");

    if (!failsafe_get_output_policy(output_id, &current))
        return failsafe_json_error(req, "404 Not Found", "invalid_output");

    enabled = current.enabled ? 1 : 0;
    boot_action = current.boot_action;
    comm_loss_action = current.comm_loss_action;
    runtime_fault_action = current.runtime_fault_action;
    safe_value = current.safe_value ? 1 : 0;
    recovery_mode = current.recovery_mode;

    if (failsafe_body_or_query_value(req, body, "enabled", enabled_text, sizeof(enabled_text)) &&
        !query_parse_int(enabled_text, 0, 1, &enabled))
    {
        return failsafe_json_error(req, "400 Bad Request", "invalid_enabled");
    }

    if (!failsafe_body_or_query_value(req, body, "boot_action", boot_text, sizeof(boot_text)))
        (void)failsafe_body_or_query_value(req, body, "startup_mode", boot_text, sizeof(boot_text));

    if (boot_text[0] != '\0' &&
        !failsafe_action_from_code(boot_text, &boot_action))
    {
        return failsafe_json_error(req, "400 Bad Request", "invalid_boot_action");
    }

    if (!failsafe_body_or_query_value(req, body, "comm_loss_action", comm_loss_text, sizeof(comm_loss_text)))
        (void)failsafe_body_or_query_value(req, body, "comm_loss_mode", comm_loss_text, sizeof(comm_loss_text));

    if (comm_loss_text[0] != '\0' &&
        !failsafe_action_from_code(comm_loss_text, &comm_loss_action))
    {
        return failsafe_json_error(req, "400 Bad Request", "invalid_comm_loss_action");
    }

    if (failsafe_body_or_query_value(req, body, "runtime_fault_action", runtime_fault_text, sizeof(runtime_fault_text)) &&
        !failsafe_action_from_code(runtime_fault_text, &runtime_fault_action))
    {
        return failsafe_json_error(req, "400 Bad Request", "invalid_runtime_fault_action");
    }

    if (failsafe_body_or_query_value(req, body, "safe_value", safe_value_text, sizeof(safe_value_text)) &&
        !query_parse_int(safe_value_text, 0, 1, &safe_value))
    {
        return failsafe_json_error(req, "400 Bad Request", "invalid_safe_value");
    }

    if (failsafe_body_or_query_value(req, body, "recovery_mode", recovery_text, sizeof(recovery_text)) &&
        !failsafe_recovery_from_code(recovery_text, &recovery_mode))
    {
        return failsafe_json_error(req, "400 Bad Request", "invalid_recovery_mode");
    }
    else if (!recovery_text[0] &&
             (failsafe_body_or_query_value(req, body, "manual_rearm", manual_text, sizeof(manual_text)) ||
              failsafe_body_or_query_value(req, body, "manual_reset_required", manual_text, sizeof(manual_text))))
    {
        if (!query_parse_int(manual_text, 0, 1, &manual_value))
            return failsafe_json_error(req, "400 Bad Request", "invalid_manual_reset");

        recovery_mode = manual_value ? FAILSAFE_RECOVERY_MANUAL : FAILSAFE_RECOVERY_AUTO;
    }

    if (!failsafe_set_output_policy(output_id,
                                    enabled != 0,
                                    boot_action,
                                    comm_loss_action,
                                    runtime_fault_action,
                                    safe_value,
                                    recovery_mode))
    {
        return failsafe_json_error(req, "500 Internal Server Error", "persist_failed");
    }

    snprintf(audit_detail,
             sizeof(audit_detail),
             "output=%u enabled=%d boot=%s comm_loss=%s runtime_fault=%s safe=%d recovery=%s",
             (unsigned)output_id,
             enabled,
             failsafe_action_to_code(boot_action),
             failsafe_action_to_code(comm_loss_action),
             failsafe_action_to_code(runtime_fault_action),
             safe_value,
             failsafe_recovery_to_code(recovery_mode));
    auth_audit_log("failsafe_policy_changed", audit_detail);
    http_server_notify_state_change();
    httpd_resp_sendstr(req, "{\"ok\":true}");
    return ESP_OK;
}

static esp_err_t failsafe_rearm_handler(httpd_req_t *req)
{
    char body[HTTP_BODY_BUFFER_SIZE] = {0};
    char audit_detail[64];
    uint16_t output_id = 0U;

    if (!http_auth_require_cap_json(req, AUTH_CAP_FAILSAFE_WRITE, false))
        return ESP_OK;

    http_set_private_json_headers(req);

    if (!http_read_request_body(req, body, sizeof(body)) && req->content_len > 0)
        return failsafe_json_error(req, "400 Bad Request", "bad_request_body");

    if (!failsafe_request_output_id(req, body, &output_id))
        return failsafe_json_error(req, "400 Bad Request", "invalid_or_missing_output_id");

    if (!failsafe_rearm(output_id))
        return failsafe_json_error(req, "404 Not Found", "invalid_output");

    snprintf(audit_detail, sizeof(audit_detail), "output=%u", (unsigned)output_id);
    auth_audit_log("failsafe_rearmed", audit_detail);
    http_server_notify_state_change();
    httpd_resp_sendstr(req, "{\"ok\":true}");
    return ESP_OK;
}

static esp_err_t failsafe_test_handler(httpd_req_t *req)
{
    char body[HTTP_BODY_BUFFER_SIZE] = {0};
    char audit_detail[96];
    uint16_t output_id = 0U;
    int32_t safe_value = 0;

    if (!http_auth_require_cap_json(req, AUTH_CAP_FAILSAFE_WRITE, false))
        return ESP_OK;

    http_set_private_json_headers(req);

    if (!http_read_request_body(req, body, sizeof(body)) && req->content_len > 0)
        return failsafe_json_error(req, "400 Bad Request", "bad_request_body");

    if (!failsafe_request_output_id(req, body, &output_id))
        return failsafe_json_error(req, "400 Bad Request", "invalid_or_missing_output_id");

    if (!failsafe_trigger_manual_test(output_id, &safe_value))
        return failsafe_json_error(req, "404 Not Found", "invalid_output");

    (void)state_set_int(output_id, safe_value);

    snprintf(audit_detail,
             sizeof(audit_detail),
             "output=%u reason=%s value=%" PRId32,
             (unsigned)output_id,
             failsafe_reason_name(FAILSAFE_REASON_MANUAL_TEST),
             safe_value);
    auth_audit_log("failsafe_test_triggered", audit_detail);
    http_server_notify_state_change();
    httpd_resp_sendstr(req, "{\"ok\":true}");
    return ESP_OK;
}

static esp_err_t wifi_status_handler(httpd_req_t *req)
{
    size_t len = build_wifi_status_json(wifi_status_json_buffer, sizeof(wifi_status_json_buffer));

    if (!http_auth_require_cap(req, AUTH_CAP_DASHBOARD_READ))
        return ESP_OK;

    http_set_public_json_headers(req);

    if (len == 0U)
        httpd_resp_send(req, "{}", 2);
    else
        httpd_resp_send(req, wifi_status_json_buffer, len);

    return ESP_OK;
}

static esp_err_t profile_handler(httpd_req_t *req)
{
    if (!http_auth_require_cap(req, AUTH_CAP_DASHBOARD_READ))
        return ESP_OK;

    char *json_buf = json_buffer_malloc(PROFILE_JSON_BUFFER_SIZE);
    if (!json_buf) { json_buffer_free(NULL); httpd_resp_send_500(req); return ESP_OK; }

    size_t len = build_profile_json(json_buf, PROFILE_JSON_BUFFER_SIZE);

    http_set_private_json_headers(req);

    if (len == 0U)
        httpd_resp_send(req, "{}", 2);
    else
        httpd_resp_send(req, json_buf, len);

    json_buffer_free(json_buf);

    return ESP_OK;
}

static esp_err_t io_map_handler(httpd_req_t *req)
{
    if (!http_auth_require_cap(req, AUTH_CAP_DASHBOARD_READ))
        return ESP_OK;

    http_set_public_json_headers(req);

    io_binding_input_view_t *inputs = malloc(sizeof(io_binding_input_view_t) * IO_BINDING_MAX_INPUTS);
    io_binding_output_view_t *outputs = malloc(sizeof(io_binding_output_view_t) * IO_BINDING_MAX_OUTPUTS);
    if (!inputs || !outputs)
    {
        free(inputs);
        free(outputs);
        httpd_resp_set_status(req, "500 Internal Server Error");
        httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"no_memory\"}");
        return ESP_OK;
    }

    int input_count = io_binding_export_inputs(inputs, IO_BINDING_MAX_INPUTS);
    int output_count = io_binding_export_outputs(outputs, IO_BINDING_MAX_OUTPUTS);

    size_t buf_size = 8192;
    char *buf = malloc(buf_size);
    if (!buf)
    {
        free(inputs);
        free(outputs);
        httpd_resp_set_status(req, "500 Internal Server Error");
        httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"no_memory\"}");
        return ESP_OK;
    }

    size_t offset = 0;
    offset += (size_t)snprintf(buf + offset, buf_size - offset, "{\"inputs\":[");

    for (int i = 0; i < input_count; i++)
    {
        int32_t val = 0;
        (void)state_get_int(inputs[i].id, &val);

        const char *type_str = (inputs[i].channel_class == DEVICE_CHANNEL_CLASS_ANALOG_INPUT) ? "analog_input" : "digital_input";
        const char *prof_str = (inputs[i].backend == DEVICE_CHANNEL_BACKEND_MCP23X17) ? "mcp23x17" : "gpio";

        int byte_idx = i / 8;
        int bit_idx = i % 8;

        if (i > 0)
            offset += (size_t)snprintf(buf + offset, buf_size - offset, ",");
        offset += (size_t)snprintf(buf + offset, buf_size - offset,
            "{\"channel_id\":%u,\"name\":\"I%d.%d\",\"type\":\"%s\",\"gpio\":%d,\"profile\":\"%s\",\"state\":%" PRId32 ",\"implemented\":true}",
            (unsigned int)inputs[i].id, byte_idx, bit_idx, type_str, inputs[i].gpio, prof_str, val);
    }

    offset += (size_t)snprintf(buf + offset, buf_size - offset, "],\"outputs\":[");

    for (int i = 0; i < output_count; i++)
    {
        int byte_idx = i / 8;
        int bit_idx = i % 8;
        int32_t desired_val = 0;
        (void)state_get_int(outputs[i].id, &desired_val);
        int32_t reported_val = desired_val;
        (void)io_driver_get_output_physical_level(outputs[i].id, &reported_val);
        bool fs_active = failsafe_is_active(outputs[i].id);

        if (i > 0)
            offset += (size_t)snprintf(buf + offset, buf_size - offset, ",");
        offset += (size_t)snprintf(buf + offset, buf_size - offset,
            "{\"channel_id\":%u,\"name\":\"Q%d.%d\",\"type\":\"digital_output\",\"gpio\":%d,\"profile\":\"relay\",\"desired\":%" PRId32 ",\"reported\":%" PRId32 ",\"failsafe\":%s,\"driver_enabled\":true,\"physical_present\":true,\"control_path\":\"V2\",\"last_transition\":\"state_set_int -> pending_mask -> gpio_set_level\"}",
            (unsigned int)outputs[i].id, byte_idx, bit_idx, outputs[i].gpio, desired_val, reported_val, fs_active ? "true" : "false");
    }


    offset += (size_t)snprintf(buf + offset, buf_size - offset, "],\"internals\":[");

    bool first_m = true;
    for (int i = 0; i < 32; i++)
    {
        uint16_t mem_id = 26 + i;
        int32_t mem_val = 0;
        (void)state_get_int(mem_id, &mem_val);

        int byte_idx = i / 8;
        int bit_idx = i % 8;

        if (!first_m)
            offset += (size_t)snprintf(buf + offset, buf_size - offset, ",");
        first_m = false;

        offset += (size_t)snprintf(buf + offset, buf_size - offset,
            "{\"channel_id\":%u,\"name\":\"M%d.%d\",\"type\":\"memory\",\"state\":%" PRId32 "}",
            (unsigned int)mem_id, byte_idx, bit_idx, mem_val);
    }

    offset += (size_t)snprintf(buf + offset, buf_size - offset, "]}");

    httpd_resp_send(req, buf, offset);
    free(buf);
    free(inputs);
    free(outputs);
    return ESP_OK;
}

static esp_err_t pve_handler(httpd_req_t *req)
{
    if (!http_auth_require_cap(req, AUTH_CAP_DASHBOARD_READ))
        return ESP_OK;

    http_set_private_json_headers(req);

    size_t buf_size = 2048;
    char *buf = malloc(buf_size);
    if (!buf)
    {
        httpd_resp_set_status(req, "500 Internal Server Error");
        httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"no_memory\"}");
        return ESP_OK;
    }

    size_t offset = 0;
    offset += (size_t)snprintf(buf + offset, buf_size - offset, "{\"variables\":[");

    uint32_t count = pve_count();
    for (uint32_t i = 0; i < count; i++)
    {
        const pve_variable_t *var = pve_get(i);
        if (!var)
            continue;

        const char *source_str = "UNKNOWN";
        switch (var->source_type)
        {
            case PVE_SOURCE_ADC_NATIVE:
                source_str = "ADC_NATIVE";
                break;
            case PVE_SOURCE_ADC_EXTERNAL:
                source_str = "ADC_EXTERNAL";
                break;
            case PVE_SOURCE_MODBUS:
                source_str = "MODBUS";
                break;
            case PVE_SOURCE_ESPNOW:
                source_str = "ESPNOW";
                break;
            case PVE_SOURCE_VIRTUAL:
                source_str = "VIRTUAL";
                break;
        }

        if (i > 0)
            offset += (size_t)snprintf(buf + offset, buf_size - offset, ",");

        offset += (size_t)snprintf(buf + offset, buf_size - offset,
            "{\"id\":%u,\"name\":\"%s\",\"source\":\"%s\",\"source_id\":%u,\"raw\":%" PRId32 ",\"scaled\":%" PRId32 ",\"scaled_value\":%" PRId32 ",\"decimals\":%u,\"unit\":\"%s\",\"input_min\":%" PRId32 ",\"input_max\":%" PRId32 ",\"scaled_min\":%" PRId32 ",\"scaled_max\":%" PRId32 ",\"alarm_enabled\":%u,\"alarm_high\":%" PRId32 ",\"alarm_low\":%" PRId32 ",\"alarm_hysteresis\":%" PRId32 ",\"alarm_state\":%u}",
            (unsigned int)i, var->name, source_str, (unsigned int)var->source_id, var->runtime.raw_value,
            var->runtime.scaled_value, var->runtime.scaled_value, (unsigned int)var->config.decimals, var->config.unit,
            var->config.input_min, var->config.input_max, var->config.scaled_min, var->config.scaled_max,
            (unsigned int)var->alarm.enabled, var->alarm.high_limit, var->alarm.low_limit, var->alarm.hysteresis, (unsigned int)var->runtime.alarm_state);

    }

    offset += (size_t)snprintf(buf + offset, buf_size - offset, "]}");

    httpd_resp_send(req, buf, offset);
    free(buf);
    return ESP_OK;
}

static esp_err_t pve_save_config_handler(httpd_req_t *req)
{
    if (!http_auth_require_cap(req, AUTH_CAP_PROFILE_WRITE))
    return ESP_OK;

    http_set_private_json_headers(req);

    char body[1024] = {0};
    if (!http_read_request_body(req, body, sizeof(body)))
    {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"bad_request\"}");
    return ESP_OK;
    }

    char id_text[16] = {0};
    if (!http_body_get_value(body, "id", id_text, sizeof(id_text)))
    {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"missing_id\"}");
    return ESP_OK;
    }

    uint32_t id = (uint32_t)atoi(id_text);
    if (id >= pve_count())
    {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"invalid_id\"}");
    return ESP_OK;
    }

    pve_variable_t *var = &pve_variables[id];

    // Validate input range (prevent division by zero)
    char input_min_text[16] = {0};
    char input_max_text[16] = {0};
    int32_t next_input_min = var->config.input_min;
    int32_t next_input_max = var->config.input_max;

    if (http_body_get_value(body, "input_min", input_min_text, sizeof(input_min_text))) {
        next_input_min = atoi(input_min_text);
    }
    if (http_body_get_value(body, "input_max", input_max_text, sizeof(input_max_text))) {
        next_input_max = atoi(input_max_text);
    }

    if (next_input_min == next_input_max) {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"input_min_equals_max\"}");
    return ESP_OK;
    }

    // Validate string lengths
    char name_temp[128] = {0};
    if (http_body_get_value(body, "name", name_temp, sizeof(name_temp))) {
        if (strlen(name_temp) >= 16) {
            httpd_resp_set_status(req, "400 Bad Request");
            httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"name_too_long\"}");
    return ESP_OK;
        }
    }

    char unit_temp[64] = {0};
    if (http_body_get_value(body, "unit", unit_temp, sizeof(unit_temp))) {
        if (strlen(unit_temp) >= 8) {
            httpd_resp_set_status(req, "400 Bad Request");
            httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"unit_too_long\"}");
    return ESP_OK;
        }
    }

    // Apply values after validations
    if (name_temp[0] != '\0') {
        strncpy(var->name, name_temp, sizeof(var->name) - 1);
    }
    var->config.input_min = next_input_min;
    var->config.input_max = next_input_max;

    char scaled_min_text[16] = {0};
    char scaled_max_text[16] = {0};
    char decimals_text[16] = {0};

    if (http_body_get_value(body, "scaled_min", scaled_min_text, sizeof(scaled_min_text))) {
        var->config.scaled_min = atoi(scaled_min_text);
    }
    if (http_body_get_value(body, "scaled_max", scaled_max_text, sizeof(scaled_max_text))) {
        var->config.scaled_max = atoi(scaled_max_text);
    }
    if (http_body_get_value(body, "decimals", decimals_text, sizeof(decimals_text))) {
        var->config.decimals = (uint8_t)atoi(decimals_text);
    }
    if (unit_temp[0] != '\0') {
        strncpy(var->config.unit, unit_temp, sizeof(var->config.unit) - 1);
    }

    // Parse and validate alarm limits
    char alarm_enabled_text[16] = {0};
    char alarm_high_text[16] = {0};
    char alarm_low_text[16] = {0};
    char alarm_hysteresis_text[16] = {0};
    uint8_t next_alarm_enabled = var->alarm.enabled;
    int32_t next_alarm_high = var->alarm.high_limit;
    int32_t next_alarm_low = var->alarm.low_limit;
    int32_t next_alarm_hysteresis = var->alarm.hysteresis;

    if (http_body_get_value(body, "alarm_enabled", alarm_enabled_text, sizeof(alarm_enabled_text))) {
        next_alarm_enabled = (uint8_t)atoi(alarm_enabled_text);
    }
    if (http_body_get_value(body, "alarm_high", alarm_high_text, sizeof(alarm_high_text))) {
        next_alarm_high = atoi(alarm_high_text);
    }
    if (http_body_get_value(body, "alarm_low", alarm_low_text, sizeof(alarm_low_text))) {
        next_alarm_low = atoi(alarm_low_text);
    }
    if (http_body_get_value(body, "alarm_hysteresis", alarm_hysteresis_text, sizeof(alarm_hysteresis_text))) {
        next_alarm_hysteresis = atoi(alarm_hysteresis_text);
    }

    if (next_alarm_hysteresis < 0) {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"invalid_hysteresis\"}");
    return ESP_OK;
    }

    if (next_alarm_enabled && (next_alarm_high <= next_alarm_low)) {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"invalid_alarm_limits\"}");
    return ESP_OK;
    }

    var->alarm.enabled = next_alarm_enabled;
    var->alarm.high_limit = next_alarm_high;
    var->alarm.low_limit = next_alarm_low;
    var->alarm.hysteresis = next_alarm_hysteresis;

    pve_update(var, var->runtime.raw_value);
    pve_save_config();

    httpd_resp_sendstr(req, "{\"ok\":true}");
    return ESP_OK;
}

static esp_err_t pve_alarms_history_handler(httpd_req_t *req)
{
    if (!http_auth_require_cap(req, AUTH_CAP_DASHBOARD_READ))
        return ESP_OK;

    http_set_private_json_headers(req);

    size_t buf_size = 4096;
    char *buf = malloc(buf_size);
    if (!buf)
    {
        httpd_resp_set_status(req, "500 Internal Server Error");
        httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"no_memory\"}");
        return ESP_OK;
    }

    size_t offset = 0;
    offset += (size_t)snprintf(buf + offset, buf_size - offset, "{\"events\":[");

    pve_alarm_event_t events[64];
    uint32_t count = pve_get_alarm_history(events, 64);

    for (uint32_t i = 0; i < count; i++)
    {
        if (i > 0)
            offset += (size_t)snprintf(buf + offset, buf_size - offset, ",");

        offset += (size_t)snprintf(buf + offset, buf_size - offset,
            "{\"timestamp\":%" PRIu32 ",\"pve_id\":%u,\"state\":%u,\"scaled_value\":%" PRId32 "}",
            events[i].timestamp, (unsigned int)events[i].pve_id, (unsigned int)events[i].state, events[i].scaled_value);
    }

    offset += (size_t)snprintf(buf + offset, buf_size - offset, "]}");

    httpd_resp_send(req, buf, offset);
    free(buf);
    return ESP_OK;
}


static esp_err_t ethernet_ip_config_get_handler(httpd_req_t *req)
{
    if (!http_auth_require_cap(req, AUTH_CAP_DASHBOARD_READ))
        return ESP_OK;

    http_set_private_json_headers(req);

    eth_ip_config_t ip_cfg;
    ethernet_manager_get_ip_config(&ip_cfg);

    char response[320];
    snprintf(response, sizeof(response),
             "{\"static_ip_enabled\":%u,\"ip\":\"%s\",\"netmask\":\"%s\",\"gateway\":\"%s\",\"dns\":\"%s\",\"dns_sec\":\"%s\"}",
             ip_cfg.static_ip_enabled, ip_cfg.ip, ip_cfg.netmask, ip_cfg.gateway, ip_cfg.dns, ip_cfg.dns_sec);

    httpd_resp_sendstr(req, response);
    return ESP_OK;
}

static esp_err_t ethernet_ip_config_post_handler(httpd_req_t *req)
{
    if (!http_auth_require_cap(req, AUTH_CAP_TRANSPORT_WRITE))
    return ESP_OK;

    http_set_private_json_headers(req);

    char body[512] = {0};
    if (!http_read_request_body(req, body, sizeof(body)))
    {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"bad_request\"}");
    return ESP_OK;
    }

    eth_ip_config_t ip_cfg = {0};

    char enabled_text[16] = {0};
    if (http_body_get_value(body, "static_ip_enabled", enabled_text, sizeof(enabled_text))) {
        ip_cfg.static_ip_enabled = (uint8_t)atoi(enabled_text);
    }
    http_body_get_value(body, "ip", ip_cfg.ip, sizeof(ip_cfg.ip));
    http_body_get_value(body, "netmask", ip_cfg.netmask, sizeof(ip_cfg.netmask));
    http_body_get_value(body, "gateway", ip_cfg.gateway, sizeof(ip_cfg.gateway));
    http_body_get_value(body, "dns", ip_cfg.dns, sizeof(ip_cfg.dns));
    http_body_get_value(body, "dns_sec", ip_cfg.dns_sec, sizeof(ip_cfg.dns_sec));

    ethernet_manager_set_ip_config(&ip_cfg);

    httpd_resp_sendstr(req, "{\"ok\":true}");
    return ESP_OK;
}

static esp_err_t resources_handler(httpd_req_t *req)
{
    if (!http_auth_require_cap(req, AUTH_CAP_DASHBOARD_READ))
        return ESP_OK;

    http_set_private_json_headers(req);

    return send_resources_json(req);
}

static esp_err_t installation_map_handler(httpd_req_t *req)
{
    if (!http_auth_require_cap(req, AUTH_CAP_DASHBOARD_READ))
        return ESP_OK;

    http_set_private_json_headers(req);

    return send_installation_map_json(req);
}

static esp_err_t installation_map_save_handler(httpd_req_t *req)
{
    char body[HTTP_BODY_BUFFER_SIZE * 2U] = {0};
    char node_id_text[24] = {0};
    char channel_id_text[16] = {0};
    char kind_text[16] = {0};
    char local_code[INSTALLATION_MAP_LOCAL_CODE_LEN] = {0};
    char global_code[INSTALLATION_MAP_GLOBAL_CODE_LEN] = {0};
    char global_id[INSTALLATION_MAP_GLOBAL_CODE_LEN] = {0};
    char alias[INSTALLATION_MAP_ALIAS_LEN] = {0};
    char room[INSTALLATION_MAP_ROOM_LEN] = {0};
    char group[INSTALLATION_MAP_GROUP_LEN] = {0};
    char visibility[INSTALLATION_MAP_VISIBILITY_LEN] = {0};
    char notes[INSTALLATION_MAP_NOTES_LEN] = {0};
    char sort_order_text[16] = {0};
    uint32_t node_id = 0U;
    uint16_t channel_id = 0U;
    uint8_t kind = 0U;
    int sort_order = 0;

    if (!http_auth_require_cap(req, AUTH_CAP_PROFILE_WRITE))
    return ESP_OK;

    http_set_private_json_headers(req);

    if (!http_read_request_body(req, body, sizeof(body)) ||
        !http_body_get_value(body, "node_id", node_id_text, sizeof(node_id_text)) ||
        !http_body_get_value(body, "channel_id", channel_id_text, sizeof(channel_id_text)) ||
        !http_body_get_value(body, "kind", kind_text, sizeof(kind_text)))
    {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"bad_request\"}");
    return ESP_OK;
    }

    (void)http_body_get_value(body, "local_code", local_code, sizeof(local_code));
    (void)http_body_get_value(body, "global_code", global_code, sizeof(global_code));
    (void)http_body_get_value(body, "global_id", global_id, sizeof(global_id));
    (void)http_body_get_value(body, "alias", alias, sizeof(alias));
    (void)http_body_get_value(body, "room", room, sizeof(room));
    (void)http_body_get_value(body, "group", group, sizeof(group));
    (void)http_body_get_value(body, "visibility", visibility, sizeof(visibility));
    (void)http_body_get_value(body, "notes", notes, sizeof(notes));
    (void)http_body_get_value(body, "sort_order", sort_order_text, sizeof(sort_order_text));

    if (global_code[0] == '\0' && global_id[0] != '\0')
        snprintf(global_code, sizeof(global_code), "%s", global_id);
    if (sort_order_text[0] != '\0')
        sort_order = atoi(sort_order_text);

    node_id = (uint32_t)strtoul(node_id_text, NULL, 10);
    channel_id = (uint16_t)strtoul(channel_id_text, NULL, 10);
    if (strcmp(kind_text, "output") == 0 || strcmp(kind_text, "1") == 0)
        kind = 1U;
    else if (strcmp(kind_text, "input") == 0 || strcmp(kind_text, "0") == 0)
        kind = 0U;
    else
    {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"invalid_kind\"}");
    return ESP_OK;
    }

    if (node_id == 0U || channel_id == 0U)
    {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"invalid_target\"}");
    return ESP_OK;
    }

    if (!installation_map_upsert(node_id,
                                 kind,
                                 channel_id,
                                 local_code,
                                 global_code,
                                 alias,
                                 room,
                                 group,
                                 (int16_t)sort_order,
                                 visibility,
                                 notes))
    {
        httpd_resp_set_status(req, "500 Internal Server Error");
        httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"installation_map_save_failed\"}");
    return ESP_OK;
    }

    auth_audit_log("installation_map_saved", local_code[0] ? local_code : kind_text);

    return send_installation_map_json(req);
}

#include "device_profile_sensors.h"
static esp_err_t sensors_config_get_handler(httpd_req_t *req)
{
    const device_sensor_profile_t *sensors = device_profile_get_sensors();
    char buf[384];
    int len = snprintf(buf, sizeof(buf),
        "{\"dht11_enabled\":%s,\"dht11_gpio\":%d,\"ds18b20_enabled\":%s,\"ds18b20_gpio\":%d,\"aht10_enabled\":%s,\"aht10_sda_gpio\":%d,\"aht10_scl_gpio\":%d}",
        sensors && sensors->dht11_enabled ? "true" : "false",
        sensors ? sensors->dht11_gpio : -1,
        sensors && sensors->ds18b20_enabled ? "true" : "false",
        sensors ? sensors->ds18b20_gpio : -1,
        sensors && sensors->aht10_enabled ? "true" : "false",
        sensors ? sensors->aht10_sda_gpio : 21,
        sensors ? sensors->aht10_scl_gpio : 22);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, buf, len);
    return ESP_OK;
}

static esp_err_t sensors_config_post_handler(httpd_req_t *req)
{
    if (!http_auth_require_cap(req, AUTH_CAP_PROFILE_WRITE))
        return ESP_OK;

    http_set_private_json_headers(req);

    char body[1024] = {0};
    (void)http_read_request_body(req, body, sizeof(body));

    device_sensor_profile_t profile = {0};
    const device_sensor_profile_t *current = device_profile_get_sensors();
    if (current) profile = *current;

    char val_buf[32] = {0};

    // AHT10
    if (http_body_get_value(body, "aht10_enabled", val_buf, sizeof(val_buf)) ||
        query_get_value(req, "aht10_enabled", val_buf, sizeof(val_buf)) == QUERY_VALUE_OK)
    {
        profile.aht10_enabled = (atoi(val_buf) != 0);
    }
    if (http_body_get_value(body, "aht10_sda_gpio", val_buf, sizeof(val_buf)) ||
        query_get_value(req, "aht10_sda_gpio", val_buf, sizeof(val_buf)) == QUERY_VALUE_OK)
    {
        profile.aht10_sda_gpio = (gpio_num_t)atoi(val_buf);
    }
    if (http_body_get_value(body, "aht10_scl_gpio", val_buf, sizeof(val_buf)) ||
        query_get_value(req, "aht10_scl_gpio", val_buf, sizeof(val_buf)) == QUERY_VALUE_OK)
    {
        profile.aht10_scl_gpio = (gpio_num_t)atoi(val_buf);
    }

    // DHT11
    if (http_body_get_value(body, "dht11_enabled", val_buf, sizeof(val_buf)) ||
        query_get_value(req, "dht11_enabled", val_buf, sizeof(val_buf)) == QUERY_VALUE_OK)
    {
        profile.dht11_enabled = (atoi(val_buf) != 0);
    }
    if (http_body_get_value(body, "dht11_gpio", val_buf, sizeof(val_buf)) ||
        query_get_value(req, "dht11_gpio", val_buf, sizeof(val_buf)) == QUERY_VALUE_OK)
    {
        profile.dht11_gpio = (gpio_num_t)atoi(val_buf);
    }

    // DS18B20
    if (http_body_get_value(body, "ds18b20_enabled", val_buf, sizeof(val_buf)) ||
        query_get_value(req, "ds18b20_enabled", val_buf, sizeof(val_buf)) == QUERY_VALUE_OK)
    {
        profile.ds18b20_enabled = (atoi(val_buf) != 0);
    }
    if (http_body_get_value(body, "ds18b20_gpio", val_buf, sizeof(val_buf)) ||
        query_get_value(req, "ds18b20_gpio", val_buf, sizeof(val_buf)) == QUERY_VALUE_OK)
    {
        profile.ds18b20_gpio = (gpio_num_t)atoi(val_buf);
    }

    if (device_profile_set_sensors(&profile)) {
        if (!profile.aht10_enabled) {
            pve_clear_variable(&pve_variables[2]);
            pve_clear_variable(&pve_variables[3]);
        }
        if (!profile.dht11_enabled) {
            pve_clear_variable(&pve_variables[5]);
            pve_clear_variable(&pve_variables[6]);
        }
        if (!profile.ds18b20_enabled) {
            pve_clear_variable(&pve_variables[4]);
        }
        http_server_notify_state_change();
        httpd_resp_sendstr(req, "{\"ok\":true}");
    } else {
        httpd_resp_set_status(req, "500 Internal Server Error");
        httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"save_failed\"}");
    }

    return ESP_OK;
}
static esp_err_t network_config_handler(httpd_req_t *req)
{
    const device_network_profile_t *network = device_profile_network();
    device_profile_network_config_result_t result;
    int wifi_enabled = 0;
    int ethernet_enabled = 0;
    int rs485_enabled = 0;
    int wifi_mode = 0;
    bool wifi_found = false;
    bool ethernet_found = false;
    bool rs485_found = false;
    bool wifi_mode_found = false;
    char audit_detail[96];

    if (!http_auth_require_cap(req, AUTH_CAP_TRANSPORT_WRITE))
        return ESP_OK;

    http_set_private_text_headers(req);

    if (!network ||
        !query_get_optional_int(req, "wifi_enabled", 0, 1, &wifi_enabled, &wifi_found) ||
        !query_get_optional_int(req, "ethernet_enabled", 0, 1, &ethernet_enabled, &ethernet_found) ||
        !query_get_optional_int(req, "rs485_enabled", 0, 1, &rs485_enabled, &rs485_found) ||
        !query_get_optional_int(req, "wifi_mode", 0, 2, &wifi_mode, &wifi_mode_found))
    {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_sendstr(req, "BAD_REQUEST");
        return ESP_OK;
    }

    if (!wifi_found && !ethernet_found && !rs485_found)
    {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_sendstr(req, "BAD_REQUEST");
        return ESP_OK;
    }

    if (!wifi_found)
        wifi_enabled = network->wifi_enabled ? 1 : 0;

    if (!ethernet_found)
        ethernet_enabled = network->ethernet_enabled ? 1 : 0;

    if (!rs485_found)
        rs485_enabled = network->rs485_enabled ? 1 : 0;

    if (!wifi_mode_found)
        wifi_mode = (int)network->wifi_mode;

    result = device_profile_set_network_enabled(wifi_enabled != 0,
                                                ethernet_enabled != 0,
                                                rs485_enabled != 0,
                                                (device_profile_wifi_mode_t)wifi_mode);
    if (result == DEVICE_PROFILE_NETWORK_CONFIG_UNSUPPORTED)
    {
        httpd_resp_set_status(req, "409 Conflict");
        httpd_resp_sendstr(req, "UNSUPPORTED_NETWORK");
        return ESP_OK;
    }

    if (result != DEVICE_PROFILE_NETWORK_CONFIG_OK)
    {
        httpd_resp_set_status(req, "500 Internal Server Error");
        httpd_resp_sendstr(req, "SAVE_FAILED");
        return ESP_OK;
    }

    snprintf(audit_detail,
             sizeof(audit_detail),
             "wifi=%d ethernet=%d rs485=%d",
             wifi_enabled,
             ethernet_enabled,
             rs485_enabled);
    auth_audit_log("transport_profile_updated", audit_detail);
    httpd_resp_sendstr(req, "SAVED_REBOOT_REQUIRED");
    return ESP_OK;
}

static esp_err_t network_preview_handler(httpd_req_t *req)
{
    const device_network_profile_t *network = device_profile_network();
    int wifi_enabled = 0;
    int ethernet_enabled = 0;
    int rs485_enabled = 0;
    bool wifi_found = false;
    bool ethernet_found = false;
    bool rs485_found = false;
    size_t len;

    if (!http_auth_require_cap(req, AUTH_CAP_TRANSPORT_WRITE))
        return ESP_OK;

    http_set_private_json_headers(req);

    if (!network ||
        !query_get_optional_int(req, "wifi_enabled", 0, 1, &wifi_enabled, &wifi_found) ||
        !query_get_optional_int(req, "ethernet_enabled", 0, 1, &ethernet_enabled, &ethernet_found) ||
        !query_get_optional_int(req, "rs485_enabled", 0, 1, &rs485_enabled, &rs485_found))
    {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_send(req, "{\"error\":\"BAD_REQUEST\"}", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }

    if (!wifi_found)
        wifi_enabled = network->wifi_enabled ? 1 : 0;

    if (!ethernet_found)
        ethernet_enabled = network->ethernet_enabled ? 1 : 0;

    if (!rs485_found)
        rs485_enabled = network->rs485_enabled ? 1 : 0;

    char *json_buf = json_buffer_malloc(NETWORK_PREVIEW_JSON_BUFFER_SIZE);
    if (!json_buf) { json_buffer_free(NULL); httpd_resp_send_500(req); return ESP_OK; }

    len = build_network_preview_json(json_buf,
                                     NETWORK_PREVIEW_JSON_BUFFER_SIZE,
                                     wifi_enabled != 0,
                                     ethernet_enabled != 0,
                                     rs485_enabled != 0);

    if (len == 0U)
        httpd_resp_send(req, "{\"error\":\"BUFFER\"}", HTTPD_RESP_USE_STRLEN);
    else
        httpd_resp_send(req, json_buf, len);

    json_buffer_free(json_buf);

    return ESP_OK;
}

static esp_err_t hardware_gpios_handler(httpd_req_t *req)
{
    if (!http_auth_require_cap(req, AUTH_CAP_DASHBOARD_READ))
        return ESP_OK;

    http_set_private_json_headers(req);

    const device_network_profile_t *net = device_profile_network();
    const device_sensor_profile_t *sensors = device_profile_get_sensors();

    /* Checagem de periféricos efetivamente ativos no runtime */
    bool rs485_runtime_active = (net && net->rs485_enabled && net->rs485_supported && device_profile_should_start_rs485_on_boot());
    bool eth_runtime_active = (net && net->ethernet_enabled && net->ethernet_supported &&
                               net->ethernet_mode == DEVICE_PROFILE_ETH_SPI_W5500 &&
                               device_profile_should_start_ethernet_on_boot() &&
                               ethernet_manager_is_ready());
    bool aht10_runtime_active = (sensors && sensors->aht10_enabled);
    bool ds18b20_runtime_active = (sensors && sensors->ds18b20_enabled && (int)sensors->ds18b20_gpio >= 0);
    bool dht11_runtime_active = (sensors && sensors->dht11_enabled && (int)sensors->dht11_gpio >= 0);

    const device_network_w5500_profile_t *w5500 = NULL;
    if (net && net->ethernet_mode == DEVICE_PROFILE_ETH_SPI_W5500 && device_profile_w5500_is_configured()) {
        w5500 = device_profile_w5500();
    }

    size_t buf_size = 8192;
    char *buf = malloc(buf_size);
    if (!buf) {
        httpd_resp_send_500(req);
        return ESP_OK;
    }

    size_t offset = 0;
    offset += snprintf(buf + offset, buf_size - offset, "{\"gpios\":[");

    bool first = true;
    for (int gpio = 0; gpio <= 39; gpio++) {
        if (gpio >= 6 && gpio <= 11) {
            continue;
        }
        if (gpio == 20 || gpio == 24 || gpio == 28 || gpio == 29 || gpio == 30 || gpio == 31 ||
            gpio == 37 || gpio == 38) {
            continue;
        }

        bool is_input = true;
        bool is_output = true;
        bool is_adc = false;
        bool available = true;
        bool recommended = true;
        const char *used_by = "null";
        const char *reason = "";
        const char *conditional_res = "null";

        if (gpio == 34 || gpio == 35 || gpio == 36 || gpio == 39) {
            is_output = false;
        }
        if (gpio == 32 || gpio == 33 || gpio == 34 || gpio == 35 || gpio == 36 || gpio == 39) {
            is_adc = true;
        }

        /* 1. Reservas Estruturais (Inalteráveis) */
        if (gpio == 1 || gpio == 3) {
            available = false;
            recommended = false;
            used_by = "\"SYSTEM\"";
            reason = "UART Console";
        } else if (gpio == 0 || gpio == 2 || gpio == 12 || gpio == 15) {
            available = false;
            recommended = false;
            used_by = "\"BOOT\"";
            reason = "Bootstrap ESP32";
        }
        /* 2. Periféricos Efetivamente Ativos no Runtime */
        else if (eth_runtime_active && w5500 && (gpio == w5500->mosi_gpio || gpio == w5500->miso_gpio ||
                                                 gpio == w5500->sclk_gpio || gpio == w5500->cs_gpio ||
                                                 gpio == w5500->int_gpio || gpio == w5500->reset_gpio)) {
            available = false;
            recommended = false;
            used_by = "\"ETHERNET\"";
            reason = "Ocupado pelo Ethernet W5500 (ativo)";
        } else if (rs485_runtime_active && (gpio == 25 || gpio == 26 || gpio == 27 || gpio == 14)) {
            available = false;
            recommended = false;
            used_by = "\"RS485\"";
            reason = "Ocupado pelo RS485 Fieldbus (ativo)";
        } else if (aht10_runtime_active && sensors && (gpio == sensors->aht10_sda_gpio || gpio == sensors->aht10_scl_gpio)) {
            available = false;
            recommended = false;
            used_by = "\"AHT10\"";
            reason = "Ocupado pelo barramento I2C (AHT10 ativo)";
        } else if (ds18b20_runtime_active && sensors && gpio == sensors->ds18b20_gpio) {
            available = false;
            recommended = false;
            used_by = "\"DS18B20\"";
            reason = "Ocupado pelo sensor 1-Wire DS18B20 (ativo)";
        } else if (dht11_runtime_active && sensors && gpio == sensors->dht11_gpio) {
            available = false;
            recommended = false;
            used_by = "\"DHT11\"";
            reason = "Ocupado pelo sensor DHT11 (ativo)";
        }
        /* 3. Reservas Condicionais (Configurados em profile, mas inativos no runtime) */
        else {
            if (w5500 && (gpio == w5500->mosi_gpio || gpio == w5500->miso_gpio ||
                          gpio == w5500->sclk_gpio || gpio == w5500->cs_gpio ||
                          gpio == w5500->int_gpio || gpio == w5500->reset_gpio)) {
                conditional_res = "\"W5500\"";
            } else if (gpio == 25 || gpio == 26 || gpio == 27 || gpio == 14) {
                conditional_res = "\"RS485\"";
            } else if (gpio == 21 || gpio == 22) {
                conditional_res = "\"I2C\"";
            } else if (gpio == 33) {
                conditional_res = "\"DS18B20\"";
            }
        }

        if (!first) {
            offset += snprintf(buf + offset, buf_size - offset, ",");
        }
        first = false;

        offset += snprintf(buf + offset, buf_size - offset,
            "{\"gpio\":%d,\"is_input\":%s,\"is_output\":%s,\"is_adc\":%s,\"available\":%s,\"recommended\":%s,\"used_by\":%s,\"reason\":\"%s\",\"conditional_reservation\":%s}",
            gpio,
            is_input ? "true" : "false",
            is_output ? "true" : "false",
            is_adc ? "true" : "false",
            available ? "true" : "false",
            recommended ? "true" : "false",
            used_by,
            reason,
            conditional_res);
    }

    offset += snprintf(buf + offset, buf_size - offset, "]}");

    httpd_resp_send(req, buf, offset);
    free(buf);
    return ESP_OK;
}

static esp_err_t i2c_scan_handler(httpd_req_t *req)
{
    if (!http_auth_require_cap(req, AUTH_CAP_DASHBOARD_READ))
        return ESP_OK;

    http_set_private_json_headers(req);

    size_t buf_size = 1024;
    char *buf = malloc(buf_size);
    if (!buf) {
        httpd_resp_send_500(req);
        return ESP_OK;
    }

    size_t offset = 0;
    offset += snprintf(buf + offset, buf_size - offset, "{\"devices\":[");

    uint8_t devices[32];
    int count = 0;
    bool first = true;
    pve_i2c_scan(devices, 32, &count);

    for (int i = 0; i < count; i++) {
        if (!first) {
            offset += snprintf(buf + offset, buf_size - offset, ",");
        }
        first = false;
        offset += snprintf(buf + offset, buf_size - offset, "%d", devices[i]);
    }

    offset += snprintf(buf + offset, buf_size - offset, "]}");
    httpd_resp_send(req, buf, offset);
    free(buf);
    return ESP_OK;
}

static esp_err_t reboot_handler(httpd_req_t *req)
{
    BaseType_t task_ok;

    if (!http_auth_require_cap(req, AUTH_CAP_REBOOT_RECOVERY))
        return ESP_OK;

    http_set_private_text_headers(req);

    task_ok = xTaskCreate(reboot_task, "esp_reboot", 2048, NULL, 5, NULL);
    if (task_ok != pdPASS)
    {
        httpd_resp_set_status(req, "500 Internal Server Error");
        httpd_resp_sendstr(req, "REBOOT_SCHEDULE_FAILED");
        return ESP_OK;
    }

    auth_audit_log("reboot_requested", "dashboard");
    httpd_resp_sendstr(req, "REBOOTING");
    return ESP_OK;
}

static esp_err_t recovery_handler(httpd_req_t *req)
{
    const device_network_profile_t *network = device_profile_network();
    cluster_metrics_t metrics = cluster_get_metrics();
    char action[24] = {0};

    if (!http_auth_require_cap(req, AUTH_CAP_REBOOT_RECOVERY))
        return ESP_OK;

    http_set_private_text_headers(req);

    if (!network || !query_get_str(req, "action", action, sizeof(action)))
    {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_sendstr(req, "BAD_REQUEST");
        return ESP_OK;
    }

    if (strcmp(action, "try_reconnect") == 0)
    {
        if (!network_transport_wifi_enabled(network))
        {
            httpd_resp_set_status(req, "409 Conflict");
            httpd_resp_sendstr(req, "WIFI_DISABLED");
            return ESP_OK;
        }

        if (wifi_manager_try_reconnect() != ESP_OK)
        {
            httpd_resp_set_status(req, "500 Internal Server Error");
            httpd_resp_sendstr(req, "RECOVERY_FAILED");
            return ESP_OK;
        }

        auth_audit_log("recovery_action", "try_reconnect");
        httpd_resp_sendstr(req, "RECOVERY_RECONNECT_REQUESTED");
        return ESP_OK;
    }

    if (strcmp(action, "re_enable_wifi") == 0)
    {
        if (!network->wifi_supported)
        {
            httpd_resp_set_status(req, "409 Conflict");
            httpd_resp_sendstr(req, "UNSUPPORTED_ACTION");
            return ESP_OK;
        }

        if (!network_transport_wifi_enabled(network))
        {
            httpd_resp_set_status(req, "409 Conflict");
            httpd_resp_sendstr(req, "WIFI_DISABLED");
            return ESP_OK;
        }

        if (wifi_manager_try_reconnect() != ESP_OK)
        {
            httpd_resp_set_status(req, "202 Accepted");
            httpd_resp_sendstr(req, "WIFI_REENABLED_REBOOT_MAY_BE_REQUIRED");
            return ESP_OK;
        }

        auth_audit_log("recovery_action", "re_enable_wifi");
        httpd_resp_sendstr(req, "WIFI_REENABLED");
        return ESP_OK;
    }

    if (strcmp(action, "force_recovery") == 0)
    {
        if (!network->wifi_supported)
        {
            httpd_resp_set_status(req, "409 Conflict");
            httpd_resp_sendstr(req, "UNSUPPORTED_ACTION");
            return ESP_OK;
        }

        if (!network_transport_wifi_enabled(network))
        {
            httpd_resp_set_status(req, "409 Conflict");
            httpd_resp_sendstr(req, "WIFI_DISABLED");
            return ESP_OK;
        }

        if (wifi_manager_force_recovery_ap() != ESP_OK)
        {
            httpd_resp_set_status(req, "500 Internal Server Error");
            httpd_resp_sendstr(req, "RECOVERY_FAILED");
            return ESP_OK;
        }

        auth_audit_log("recovery_action", "force_recovery");
        httpd_resp_sendstr(req, "RECOVERY_MODE_ACTIVE");
        return ESP_OK;
    }

    if (strcmp(action, "identify") == 0)
    {
        ESP_LOGW(TAG,
                 "Identify solicitado para node %" PRIu32 " (SSID atual/local: ver /api/status)",
                 metrics.self_node);
        auth_audit_log("recovery_action", "identify");
        httpd_resp_sendstr(req, "IDENTIFY_REQUESTED");
        return ESP_OK;
    }

    httpd_resp_set_status(req, "400 Bad Request");
    httpd_resp_sendstr(req, "BAD_REQUEST");
    return ESP_OK;
}

static esp_err_t cluster_status_handler(httpd_req_t *req)
{
    if (!http_auth_require_cap(req, AUTH_CAP_DASHBOARD_READ))
        return ESP_OK;

    cluster_metrics_t metrics = cluster_get_metrics();
    const char *transport_name = cluster_transport_active_name();
    if (!transport_name)
    {
        transport_name = "none";
    }

    char *json_buf = json_buffer_malloc(2048);
    if (!json_buf) { json_buffer_free(NULL); httpd_resp_send_500(req); return ESP_OK; }

    size_t offset = 0;
    append_format(json_buf, 2048, &offset,
        "{\"node_id\":%" PRIu32 ",\"transport\":",
        metrics.self_node);
    append_json_string(json_buf, 2048, &offset, transport_name);
    append_format(json_buf, 2048, &offset,
        ",\"online\":%" PRIu32 ",\"offline\":%" PRIu32 ",\"suspect\":%" PRIu32 ",\"peers\":[",
        metrics.online, metrics.offline, metrics.suspect);

    // Export individual peers
    cluster_node_t peers[MAX_NODES];
    int count = cluster_manager_export_nodes(peers, MAX_NODES);
    int peer_count = 0;
    for (int i = 0; i < count; i++)
    {
        if (peers[i].node_id == 0 || peers[i].node_id == metrics.self_node)
            continue;

        if (peer_count > 0)
        {
            append_text(json_buf, 2048, &offset, ",");
        }

        const char *state_str = "offline";
        if (peers[i].state == CLUSTER_NODE_ONLINE) state_str = "online";
        else if (peers[i].state == CLUSTER_NODE_SUSPECT) state_str = "suspect";

        // Query IP from node registry if available, else format as empty
        node_registry_entry_t reg_entries[NODE_REGISTRY_MAX_NODES];
        int reg_count = node_registry_export(reg_entries, NODE_REGISTRY_MAX_NODES);
        char ip_text[20] = "";
        for (int r = 0; r < reg_count; r++)
        {
            if (reg_entries[r].node_id == peers[i].node_id && reg_entries[r].last_ip_addr != 0)
            {
                ip4_addr_t ip = { .addr = reg_entries[r].last_ip_addr };
                ip4addr_ntoa_r(&ip, ip_text, sizeof(ip_text));
                break;
            }
        }

        uint32_t last_seen_ms = peers[i].last_seen_ms;
        const char *peer_transport = peers[i].last_seen_ms ? transport_name : "none"; // Fallback to active runtime transport if seen
        append_format(json_buf, 2048, &offset,
            "{\"node_id\":%" PRIu32 ",\"ip\":",
            peers[i].node_id);
        append_json_string(json_buf, 2048, &offset, ip_text);
        append_format(json_buf, 2048, &offset, ",\"state\":");
        append_json_string(json_buf, 2048, &offset, state_str);
        append_format(json_buf, 2048, &offset, ",\"last_seen_ms\":%" PRIu32 ",\"transport\":", last_seen_ms);
        append_json_string(json_buf, 2048, &offset, peer_transport);
        append_text(json_buf, 2048, &offset, ",\"version\":\"1.0.0\"}");
        peer_count++;
    }

    append_text(json_buf, 2048, &offset, "]}");

    http_set_private_json_headers(req);
    httpd_resp_send(req, json_buf, offset);
    json_buffer_free(json_buf);

    return ESP_OK;
}

static esp_err_t nodes_handler(httpd_req_t *req)
{
    if (!http_auth_require_cap(req, AUTH_CAP_DASHBOARD_READ))
        return ESP_OK;

    char *json_buf = json_buffer_malloc(NODES_JSON_BUFFER_SIZE);
    if (!json_buf) { json_buffer_free(NULL); httpd_resp_send_500(req); return ESP_OK; }

    size_t len = build_nodes_json(json_buf, NODES_JSON_BUFFER_SIZE);

    http_set_public_json_headers(req);

    if (len == 0U)
        httpd_resp_send(req, "{\"nodes\":[]}", HTTPD_RESP_USE_STRLEN);
    else
        httpd_resp_send(req, json_buf, len);

    json_buffer_free(json_buf);

    return ESP_OK;
}

static esp_err_t respond_binding_result(httpd_req_t *req, io_binding_result_t result)
{
    switch (result)
    {
        case IO_BINDING_RESULT_OK:
            return ESP_OK;

        case IO_BINDING_RESULT_INVALID_GPIO:
            httpd_resp_set_status(req, "409 Conflict");
            httpd_resp_sendstr(req, "INVALID_GPIO");
            return ESP_FAIL;

        case IO_BINDING_RESULT_GPIO_CONFLICT:
            httpd_resp_set_status(req, "409 Conflict");
            httpd_resp_sendstr(req, "GPIO_CONFLICT");
            return ESP_FAIL;

        case IO_BINDING_RESULT_ALREADY_ACTIVE:
            httpd_resp_set_status(req, "409 Conflict");
            httpd_resp_sendstr(req, "ALREADY_ACTIVE");
            return ESP_FAIL;

        case IO_BINDING_RESULT_LIMIT_REACHED:
            httpd_resp_set_status(req, "409 Conflict");
            httpd_resp_sendstr(req, "LIMIT_REACHED");
            return ESP_FAIL;

        case IO_BINDING_RESULT_PROTECTED:
            httpd_resp_set_status(req, "409 Conflict");
            httpd_resp_sendstr(req, "PROTECTED_SLOT");
            return ESP_FAIL;

        case IO_BINDING_RESULT_NOT_FOUND:
        default:
            httpd_resp_set_status(req, "404 Not Found");
            httpd_resp_sendstr(req, "NOT_FOUND");
            return ESP_FAIL;
    }
}

static esp_err_t output_config_handler(httpd_req_t *req)
{
    int id = 0;
    int gpio = -1;
    char name[IO_BINDING_NAME_LEN] = {0};
    char role[IO_BINDING_ROLE_LEN] = {0};
    io_binding_result_t result;
    char audit_detail[96];

    if (!http_auth_require_cap(req, AUTH_CAP_PROFILE_WRITE))
        return ESP_OK;

    http_set_private_text_headers(req);

    if (!query_get_int(req, "id", 0, UINT16_MAX, &id) ||
        !query_get_optional_str(req, "name", name, sizeof(name), NULL) ||
        !query_get_optional_str(req, "role", role, sizeof(role), NULL) ||
        !query_get_optional_int(req, "gpio", -1, INT16_MAX, &gpio, NULL))
    {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_sendstr(req, "BAD_REQUEST");
        return ESP_OK;
    }

    result = io_binding_set_output((uint16_t)id, name, role, gpio);

    if (respond_binding_result(req, result) != ESP_OK)
        return ESP_OK;

    snprintf(audit_detail, sizeof(audit_detail), "output=%d gpio=%d name=%s", id, gpio, name[0] ? name : "-");
    auth_audit_log("output_config_updated", audit_detail);
    http_server_notify_state_change();
    httpd_resp_sendstr(req, "SAVED");
    return ESP_OK;
}

static esp_err_t input_config_handler(httpd_req_t *req)
{
    int id = 0;
    int gpio = -1;
    char name[IO_BINDING_NAME_LEN] = {0};
    char role[IO_BINDING_ROLE_LEN] = {0};
    io_binding_result_t result;
    char audit_detail[96];

    if (!http_auth_require_cap(req, AUTH_CAP_PROFILE_WRITE))
        return ESP_OK;

    http_set_private_text_headers(req);

    if (!query_get_int(req, "id", 0, UINT16_MAX, &id) ||
        !query_get_optional_str(req, "name", name, sizeof(name), NULL) ||
        !query_get_optional_str(req, "role", role, sizeof(role), NULL) ||
        !query_get_optional_int(req, "gpio", -1, INT16_MAX, &gpio, NULL))
    {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_sendstr(req, "BAD_REQUEST");
        return ESP_OK;
    }

    result = io_binding_set_input((uint16_t)id, name, role, gpio);

    if (respond_binding_result(req, result) != ESP_OK)
        return ESP_OK;

    snprintf(audit_detail, sizeof(audit_detail), "input=%d gpio=%d name=%s", id, gpio, name[0] ? name : "-");
    auth_audit_log("input_config_updated", audit_detail);
    http_server_notify_state_change();
    httpd_resp_sendstr(req, "SAVED");
    return ESP_OK;
}


static esp_err_t resource_actuate_handler(httpd_req_t *req)
{
    char body[HTTP_BODY_BUFFER_SIZE];
    int id = 0, value = 0;
    uint32_t target_node = 0U;
    bool target_found = true;
    bool explicit_remote_target = false;
    cluster_metrics_t metrics = {0};
    char audit_detail[96];
    const char *resource_name = "Unknown";
    int gpio_num = -1;

    if (!http_auth_require_cap(req, AUTH_CAP_MANUAL_IO))
        return ESP_OK;

    http_set_private_text_headers(req);

    if (!http_read_request_body(req, body, sizeof(body))) {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_sendstr(req, "BAD_REQUEST");
        return ESP_OK;
    }

    cJSON *json = cJSON_Parse(body);
    if (!json) {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_sendstr(req, "INVALID_JSON");
        return ESP_OK;
    }

    cJSON *res_id_item = cJSON_GetObjectItem(json, "resource_id");
    cJSON *val_item = cJSON_GetObjectItem(json, "value");

    if (!cJSON_IsString(res_id_item) || !cJSON_IsNumber(val_item)) {
        cJSON_Delete(json);
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_sendstr(req, "MISSING_FIELDS");
        return ESP_OK;
    }

    const char *resource_id_buf = res_id_item->valuestring;
    value = val_item->valueint;

    installation_map_entry_t *entry = installation_map_find_by_resource_id(resource_id_buf);
    if (!entry || entry->kind != 1) // 1 == digital_output
    {
        ESP_LOGE("MANUAL_IO", "FAIL: RESOURCE_NOT_FOUND (%s)", resource_id_buf);
        cJSON_Delete(json);
        httpd_resp_set_status(req, "404 Not Found");
        httpd_resp_sendstr(req, "RESOURCE_NOT_FOUND");
        return ESP_OK;
    }

    resource_name = entry->alias[0] != '\0' ? entry->alias : resource_id_buf;
    id = entry->channel_id;
    target_node = entry->node_id;
    cJSON_Delete(json);

    if (!device_profile_is_valid_output((uint16_t)id))
    {
        ESP_LOGE("MANUAL_IO", "FAIL: BINDING_NOT_FOUND (Output %d)", id);
        httpd_resp_set_status(req, "404 Not Found");
        httpd_resp_sendstr(req, "NOT_FOUND");
        return ESP_OK;
    }

    metrics = cluster_get_metrics();
    explicit_remote_target = target_found &&
                             target_node != 0U &&
                             metrics.self_node != 0U &&
                             target_node != metrics.self_node;

    if (!explicit_remote_target)
    {
        const device_output_profile_t *op = device_profile_find_output((uint16_t)id);
        if (op) {
            gpio_num = op->gpio;
            if (op->gpio == (gpio_num_t)-1) {
                ESP_LOGE("MANUAL_IO", "FAIL: GPIO_RESERVED (Output %d has no GPIO)", id);
            }
        }
    }

    ESP_LOGI("MANUAL_IO", "\n"
             "MANUAL_IO:\n"
             "Resource:\n%s\n"
             "Resource ID:\n%s\n"
             "Channel:\nOutput %d\n"
             "GPIO:\n%d\n"
             "Command:\n%s",
             resource_name,
             resource_id_buf,
             id,
             gpio_num,
             value ? "ON" : "OFF");

    if (explicit_remote_target ||
        !cluster_io_is_local((uint16_t)id))
    {
        protocol_msg_t msg = {0};
        uint32_t owner = 0U;

        if (metrics.self_node == 0U)
        {
            ESP_LOGE("MANUAL_IO", "FAIL: SELF_NODE_UNAVAILABLE");
            httpd_resp_set_status(req, "409 Conflict");
            httpd_resp_sendstr(req, "SELF_NODE_UNAVAILABLE");
            return ESP_OK;
        }

        owner = (target_found && target_node != 0U) ? target_node : cluster_io_get_owner((uint16_t)id);

        if (owner == 0U || owner == metrics.self_node)
        {
            ESP_LOGE("MANUAL_IO", "FAIL: REMOTE_OWNER_UNRESOLVED");
            httpd_resp_set_status(req, "409 Conflict");
            httpd_resp_sendstr(req, "REMOTE_OWNER_UNRESOLVED");
            return ESP_OK;
        }

        if (!node_registry_is_operational(owner))
        {
            ESP_LOGE("MANUAL_IO", "FAIL: REMOTE_OWNER_OFFLINE");
            httpd_resp_set_status(req, "409 Conflict");
            httpd_resp_sendstr(req, "REMOTE_OWNER_OFFLINE");
            return ESP_OK;
        }

        if (!cluster_transport_is_ready())
        {
            ESP_LOGE("MANUAL_IO", "FAIL: TRANSPORT_NOT_READY");
            httpd_resp_set_status(req, "503 Service Unavailable");
            httpd_resp_sendstr(req, "TRANSPORT_NOT_READY");
            return ESP_OK;
        }

        msg.type = PROTOCOL_MSG_OUTPUT_COMMAND;
        msg.data.output_command.target_node = owner;
        msg.data.output_command.requester_node = metrics.self_node;
        msg.data.output_command.output_id = (uint16_t)id;
        msg.data.output_command.value = value;

        if (!cluster_transport_broadcast_frame((const uint8_t *)&msg, sizeof(msg)))
        {
            ESP_LOGE("MANUAL_IO", "FAIL: DISPATCH_FAILED");
            httpd_resp_set_status(req, "503 Service Unavailable");
            httpd_resp_sendstr(req, "DISPATCH_FAILED");
            return ESP_OK;
        }

        snprintf(audit_detail,
                 sizeof(audit_detail),
                 "output=%d value=%d target=%" PRIu32 "%s",
                 id,
                 value,
                 owner,
                 target_found ? " explicit" : "");
        auth_audit_log("manual_output_command_remote", audit_detail);
        http_server_notify_state_change();
        ESP_LOGI("MANUAL_IO", "Result:\nSUCCESS (REMOTE DISPATCHED)");
        httpd_resp_set_status(req, "202 Accepted");
        httpd_resp_sendstr(req, "DISPATCHED_REMOTE");
        return ESP_OK;
    }

    int32_t effective_value = value;
    const char *failsafe_reason = NULL;

    if (!failsafe_guard_command((uint16_t)id,
                                value,
                                FAILSAFE_COMMAND_MANUAL,
                                &effective_value,
                                &failsafe_reason))
    {
        snprintf(audit_detail,
                 sizeof(audit_detail),
                 "output=%d value=%d blocked reason=%s",
                 id,
                 value,
                 failsafe_reason ? failsafe_reason : "unknown");
        auth_audit_log("manual_output_blocked_failsafe", audit_detail);
        ESP_LOGE("MANUAL_IO", "FAIL: RUNTIME_REJECTED (FAILSAFE_ACTIVE)");
        httpd_resp_set_status(req, "409 Conflict");
        httpd_resp_sendstr(req, "FAILSAFE_ACTIVE");
        return ESP_OK;
    }

    if (!io_command_push(id, effective_value))
    {
        ESP_LOGE("MANUAL_IO", "FAIL: RUNTIME_REJECTED (BUSY)");
        httpd_resp_set_status(req, "503 Service Unavailable");
        httpd_resp_sendstr(req, "BUSY");
        return ESP_OK;
    }

    snprintf(audit_detail, sizeof(audit_detail), "output=%d value=%d", id, (int)effective_value);
    auth_audit_log("manual_output_command", audit_detail);
    http_server_notify_state_change();
    ESP_LOGI("MANUAL_IO", "Result:\nSUCCESS");
    httpd_resp_sendstr(req, "OK");
    return ESP_OK;
}

static esp_err_t input_reset_handler(httpd_req_t *req)
{
    int id = 0;
    io_binding_result_t result;

    if (!http_auth_require_cap(req, AUTH_CAP_PROFILE_WRITE))
        return ESP_OK;

    http_set_private_text_headers(req);

    if (!query_get_int(req, "id", 0, UINT16_MAX, &id))
    {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_sendstr(req, "BAD_REQUEST");
        return ESP_OK;
    }

    result = io_binding_reset_input((uint16_t)id);

    if (respond_binding_result(req, result) != ESP_OK)
        return ESP_OK;

    auth_audit_log("input_reset", "binding-reset");
    http_server_notify_state_change();
    httpd_resp_sendstr(req, "RESET");
    return ESP_OK;
}

static esp_err_t output_reset_handler(httpd_req_t *req)
{
    int id = 0;
    io_binding_result_t result;

    if (!http_auth_require_cap(req, AUTH_CAP_PROFILE_WRITE))
        return ESP_OK;

    http_set_private_text_headers(req);

    if (!query_get_int(req, "id", 0, UINT16_MAX, &id))
    {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_sendstr(req, "BAD_REQUEST");
        return ESP_OK;
    }

    result = io_binding_reset_output((uint16_t)id);

    if (respond_binding_result(req, result) != ESP_OK)
        return ESP_OK;

    auth_audit_log("output_reset", "binding-reset");
    http_server_notify_state_change();
    httpd_resp_sendstr(req, "RESET");
    return ESP_OK;
}

static esp_err_t input_add_handler(httpd_req_t *req)
{
    int id = 0;
    int gpio = -1;
    char name[IO_BINDING_NAME_LEN] = {0};
    char role[IO_BINDING_ROLE_LEN] = {0};
    io_binding_result_t result;
    char audit_detail[96];

    if (!http_auth_require_cap(req, AUTH_CAP_PROFILE_WRITE))
        return ESP_OK;

    http_set_private_text_headers(req);

    if (!query_get_int(req, "id", 0, UINT16_MAX, &id) ||
        !query_get_int(req, "gpio", 0, INT16_MAX, &gpio) ||
        !query_get_optional_str(req, "name", name, sizeof(name), NULL) ||
        !query_get_optional_str(req, "role", role, sizeof(role), NULL))
    {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_sendstr(req, "BAD_REQUEST");
        return ESP_OK;
    }

    result = io_binding_add_input((uint16_t)id, name, role, gpio);

    if (respond_binding_result(req, result) != ESP_OK)
        return ESP_OK;

    snprintf(audit_detail, sizeof(audit_detail), "input=%d gpio=%d", id, gpio);
    auth_audit_log("input_added", audit_detail);
    http_server_notify_state_change();
    httpd_resp_sendstr(req, "ADDED");
    return ESP_OK;
}

static esp_err_t output_add_handler(httpd_req_t *req)
{
    int id = 0;
    int gpio = -1;
    char name[IO_BINDING_NAME_LEN] = {0};
    char role[IO_BINDING_ROLE_LEN] = {0};
    io_binding_result_t result;
    char audit_detail[96];

    if (!http_auth_require_cap(req, AUTH_CAP_PROFILE_WRITE))
        return ESP_OK;

    http_set_private_text_headers(req);

    if (!query_get_int(req, "id", 0, UINT16_MAX, &id) ||
        !query_get_int(req, "gpio", 0, INT16_MAX, &gpio) ||
        !query_get_optional_str(req, "name", name, sizeof(name), NULL) ||
        !query_get_optional_str(req, "role", role, sizeof(role), NULL))
    {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_sendstr(req, "BAD_REQUEST");
        return ESP_OK;
    }

    result = io_binding_add_output((uint16_t)id, name, role, gpio);

    if (respond_binding_result(req, result) != ESP_OK)
        return ESP_OK;

    snprintf(audit_detail, sizeof(audit_detail), "output=%d gpio=%d", id, gpio);
    auth_audit_log("output_added", audit_detail);
    http_server_notify_state_change();
    httpd_resp_sendstr(req, "ADDED");
    return ESP_OK;
}

static esp_err_t input_remove_handler(httpd_req_t *req)
{
    int id = 0;
    io_binding_result_t result;

    if (!http_auth_require_cap(req, AUTH_CAP_PROFILE_WRITE))
        return ESP_OK;

    http_set_private_text_headers(req);

    if (!query_get_int(req, "id", 0, UINT16_MAX, &id))
    {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_sendstr(req, "BAD_REQUEST");
        return ESP_OK;
    }

    result = io_binding_remove_input((uint16_t)id);

    if (respond_binding_result(req, result) != ESP_OK)
        return ESP_OK;

    auth_audit_log("input_removed", "binding-removed");
    http_server_notify_state_change();
    httpd_resp_sendstr(req, "REMOVED");
    return ESP_OK;
}

static esp_err_t output_remove_handler(httpd_req_t *req)
{
    int id = 0;
    io_binding_result_t result;

    if (!http_auth_require_cap(req, AUTH_CAP_PROFILE_WRITE))
        return ESP_OK;

    http_set_private_text_headers(req);

    if (!query_get_int(req, "id", 0, UINT16_MAX, &id))
    {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_sendstr(req, "BAD_REQUEST");
        return ESP_OK;
    }

    result = io_binding_remove_output((uint16_t)id);

    if (respond_binding_result(req, result) != ESP_OK)
        return ESP_OK;

    auth_audit_log("output_removed", "binding-removed");
    http_server_notify_state_change();
    httpd_resp_sendstr(req, "REMOVED");
    return ESP_OK;
}

static esp_err_t input_learn_handler(httpd_req_t *req)
{
    char action[16] = {0};
    bool action_found = false;

    if (!http_auth_require_cap(req, AUTH_CAP_RUNTIME_DIAGNOSTICS))
        return ESP_OK;

    http_set_private_text_headers(req);

    if (!query_get_optional_str(req, "action", action, sizeof(action), &action_found))
    {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_sendstr(req, "BAD_REQUEST");
        return ESP_OK;
    }

    if (!action_found)
        memcpy(action, "arm", sizeof("arm"));

    if (strcmp(action, "arm") == 0)
    {
        input_learning_arm();
        auth_audit_log("input_learn", "arm");
        http_server_notify_state_change();
        httpd_resp_sendstr(req, "ARMED");
        return ESP_OK;
    }

    if (strcmp(action, "cancel") == 0)
    {
        input_learning_cancel();
        auth_audit_log("input_learn", "cancel");
        http_server_notify_state_change();
        httpd_resp_sendstr(req, "CANCELLED");
        return ESP_OK;
    }

    if (strcmp(action, "clear") == 0)
    {
        input_learning_clear();
        auth_audit_log("input_learn", "clear");
        http_server_notify_state_change();
        httpd_resp_sendstr(req, "CLEARED");
        return ESP_OK;
    }

    httpd_resp_set_status(req, "400 Bad Request");
    httpd_resp_sendstr(req, "INVALID_ACTION");
    return ESP_OK;
}

static esp_err_t cluster_selftest_handler(httpd_req_t *req)
{
    if (!http_auth_require_cap(req, AUTH_CAP_RUNTIME_DIAGNOSTICS))
        return ESP_OK;

    http_set_private_text_headers(req);

    if (!cluster_self_test_trigger())
    {
        httpd_resp_set_status(req, "409 Conflict");
        httpd_resp_sendstr(req, "UNAVAILABLE");
        return ESP_OK;
    }

    auth_audit_log("cluster_self_test", "started");
    http_server_notify_state_change();
    httpd_resp_sendstr(req, "STARTED");
    return ESP_OK;
}

static esp_err_t network_metrics_handler(httpd_req_t *req)
{
    if (!http_auth_require_cap(req, AUTH_CAP_DASHBOARD_READ))
        return ESP_OK;

    http_set_private_json_headers(req);

    endap_network_metrics_t m;
    endap_network_v1_get_metrics(&m);

    const char *state_str = endap_network_v1_state_name((endap_network_state_t)m.current_state);
    const char *transport_str = cluster_transport_name((cluster_transport_type_t)m.active_transport);

    char buf[384];
    snprintf(buf, sizeof(buf),
             "{\"state\":\"%s\",\"state_id\":%u,\"active_transport\":\"%s\",\"transport_id\":%u,"
             "\"packets_sent\":%" PRIu32 ",\"packets_received\":%" PRIu32 ",\"packet_drops\":%" PRIu32 ","
             "\"last_rtt_ms\":%" PRIu32 ",\"max_jitter_ms\":%" PRIu32 ",\"failover_count\":%" PRIu32 "}",
             state_str,
             (unsigned)m.current_state,
             transport_str ? transport_str : "NONE",
             (unsigned)m.active_transport,
             m.packets_sent,
             m.packets_received,
             m.packet_drops,
             m.last_rtt_ms,
             m.max_jitter_ms,
             m.failover_count);

    httpd_resp_sendstr(req, buf);
    return ESP_OK;
}

static esp_err_t onboarding_status_handler(httpd_req_t *req)
{
    if (!http_auth_require_cap(req, AUTH_CAP_DASHBOARD_READ))
        return ESP_OK;

    http_set_public_json_headers(req);

    endap_onboarding_config_t cfg;
    endap_onboarding_get_config(&cfg);

    char buf[384];
    snprintf(buf, sizeof(buf),
             "{\"state\":\"%s\",\"profile\":%d,\"gateway_id\":%" PRIu32 ",\"timestamp\":%" PRIu32 ",\"node_name\":\"%s\"}",
             endap_onboarding_state_name((endap_onboarding_state_t)cfg.state),
             cfg.profile,
             cfg.adopted_by_gateway_id,
             cfg.adopted_timestamp,
             cfg.node_name);

    httpd_resp_sendstr(req, buf);
    return ESP_OK;
}

static esp_err_t onboarding_claim_handler(httpd_req_t *req)
{
    if (!http_auth_require_cap(req, AUTH_CAP_NODE_ADMISSION))
        return ESP_OK;

    http_set_public_json_headers(req);

    char body[HTTP_BODY_BUFFER_SIZE] = {0};
    if (!http_read_request_body(req, body, sizeof(body)))
    {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"bad_request\"}");
        return ESP_OK;
    }

    char target_str[24] = {0};
    char profile_str[16] = {0};
    char gw_str[24] = {0};
    char name_str[ENDAP_NODE_NAME_MAX] = {0};

    http_body_get_value(body, "target_node_id", target_str, sizeof(target_str));
    http_body_get_value(body, "profile", profile_str, sizeof(profile_str));
    http_body_get_value(body, "gateway_id", gw_str, sizeof(gw_str));
    http_body_get_value(body, "node_name", name_str, sizeof(name_str));

    uint32_t target_node_id = (uint32_t)strtoul(target_str, NULL, 10);
    node_profile_t profile = (node_profile_t)atoi(profile_str);
    uint32_t gateway_id = (uint32_t)strtoul(gw_str, NULL, 10);

    // Correção 1: Diferenciação de Claim Local vs Remoto
    uint32_t self_id = node_identity_get();
    if (target_node_id != 0U && target_node_id != self_id)
    {
        ESP_LOGI(TAG, "Solicitando Claim Remoto para target_node_id=%" PRIu32, target_node_id);
        uint32_t gw_id = (gateway_id != 0U) ? gateway_id : self_id;
        
        bool ok = cluster_transport_send_remote_claim(target_node_id, (uint8_t)profile, gw_id, name_str, 3500U);
        if (!ok)
        {
            ESP_LOGE(TAG, "Falha ou timeout no Claim Remoto do no %" PRIu32, target_node_id);
            httpd_resp_set_status(req, "504 Gateway Timeout");
            httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"remote_claim_timeout_or_failed\"}");
            return ESP_OK;
        }

        // Atualiza o node_registry local do Gateway
        node_registry_adopt(target_node_id);
        const char *prof_str = (profile == NODE_PROFILE_GATEWAY) ? "gateway" :
                              (profile == NODE_PROFILE_FIELD) ? "field-node" :
                              (profile == NODE_PROFILE_RELAY) ? "relay-node" :
                              (profile == NODE_PROFILE_SENSOR) ? "sensor-node" : "custom";
        node_registry_configure(target_node_id, prof_str, name_str[0] ? name_str : "field_node");
        node_registry_activate(target_node_id);

        http_server_notify_state_change();
        httpd_resp_sendstr(req, "{\"ok\":true}");
        return ESP_OK;
    }

    // Execução do claim no nó LOCAL
    esp_err_t err = endap_onboarding_claim(profile, gateway_id, name_str);
    if (err != ESP_OK)
    {
        httpd_resp_set_status(req, "500 Internal Server Error");
        httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"claim_failed\"}");
        return ESP_OK;
    }

    http_server_notify_state_change();
    httpd_resp_sendstr(req, "{\"ok\":true}");
    return ESP_OK;
}

static esp_err_t onboarding_reset_handler(httpd_req_t *req)
{
    if (!http_auth_require_cap(req, AUTH_CAP_NODE_ADMISSION))
        return ESP_OK;

    http_set_public_json_headers(req);

    esp_err_t err = endap_onboarding_reset();
    if (err != ESP_OK)
    {
        httpd_resp_set_status(req, "500 Internal Server Error");
        httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"reset_failed\"}");
        return ESP_OK;
    }

    http_server_notify_state_change();
    httpd_resp_sendstr(req, "{\"ok\":true}");
    return ESP_OK;
}

static esp_err_t system_factory_reset_handler(httpd_req_t *req)
{
    if (!http_auth_require_cap(req, AUTH_CAP_NODE_ADMISSION))
        return ESP_OK;

    http_set_private_json_headers(req);

    esp_err_t err = endap_factory_reset_full();
    if (err != ESP_OK)
    {
        httpd_resp_set_status(req, "500 Internal Server Error");
        httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"factory_reset_failed\"}");
        return ESP_OK;
    }

    auth_audit_log("system_factory_reset", "full");
    http_server_notify_state_change();
    httpd_resp_sendstr(req, "{\"ok\":true}");
    return ESP_OK;
}

static uint8_t map_transport_str(const char *str)
{
    if (!str) return DEVICE_PROFILE_TRANSPORT_NONE;
    if (strcmp(str, "wifi-udp") == 0) return DEVICE_PROFILE_TRANSPORT_WIFI;
    if (strcmp(str, "ethernet-udp") == 0) return DEVICE_PROFILE_TRANSPORT_ETHERNET;
    if (strcmp(str, "rs485") == 0) return DEVICE_PROFILE_TRANSPORT_RS485;
    if (strcmp(str, "wifi-now") == 0) return DEVICE_PROFILE_TRANSPORT_ESPNOW;
    if (strcmp(str, "wifi-mesh") == 0) return DEVICE_PROFILE_TRANSPORT_MESH;
    return DEVICE_PROFILE_TRANSPORT_NONE;
}

static esp_err_t nodes_transport_handler(httpd_req_t *req)
{
    if (!http_auth_require_cap(req, AUTH_CAP_TRANSPORT_WRITE))
        return ESP_OK;

    size_t len = httpd_req_get_hdr_value_len(req, "Content-Length");
    if (len == 0 || len > 2048) {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_sendstr(req, "{\"error\":\"Missing body\"}");
        return ESP_OK;
    }
    char *buf = malloc(len + 1);
    if (!buf) return ESP_FAIL;
    int r = httpd_req_recv(req, buf, len);
    if (r <= 0) { free(buf); return ESP_FAIL; }
    buf[r] = '\0';

    cJSON *root = cJSON_Parse(buf);
    free(buf);
    if (!root) {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_sendstr(req, "{\"error\":\"Invalid JSON\"}");
        return ESP_OK;
    }

    uint32_t node_id = 0;
    if (sscanf(req->uri, "/api/nodes/%" SCNu32 "/transport", &node_id) != 1) {
        cJSON_Delete(root);
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_sendstr(req, "{\"error\":\"Invalid node_id\"}");
        return ESP_OK;
    }

    if (!node_registry_is_known(node_id)) {
        cJSON_Delete(root);
        httpd_resp_set_status(req, "404 Not Found");
        httpd_resp_sendstr(req, "{\"error\":\"Node not found\"}");
        return ESP_OK;
    }

    node_registry_state_t st = node_registry_get_state(node_id);
    if (st != NODE_REGISTRY_STATE_ACTIVE) {
        cJSON_Delete(root);
        httpd_resp_set_status(req, "409 Conflict");
        httpd_resp_sendstr(req, "{\"error\":\"Node not active\"}");
        return ESP_OK;
    }

    cJSON *primary = cJSON_GetObjectItem(root, "primary");
    cJSON *fallback = cJSON_GetObjectItem(root, "fallback");
    cJSON *wifi_mode = cJSON_GetObjectItem(root, "wifi_mode");
    cJSON *wifi_en = cJSON_GetObjectItem(root, "wifi_enabled");
    cJSON *eth_en = cJSON_GetObjectItem(root, "ethernet_enabled");
    cJSON *rs485_en = cJSON_GetObjectItem(root, "rs485_enabled");

    if (!primary || !cJSON_IsString(primary)) {
        cJSON_Delete(root);
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_sendstr(req, "{\"error\":\"Missing primary\"}");
        return ESP_OK;
    }

    uint8_t p = map_transport_str(primary->valuestring);
    uint8_t f = (fallback && cJSON_IsString(fallback))
        ? map_transport_str(fallback->valuestring)
        : DEVICE_PROFILE_TRANSPORT_NONE;

    uint8_t wm = DEVICE_PROFILE_WIFI_MODE_INFRA;
    if (wifi_mode && cJSON_IsString(wifi_mode)) {
        if (strcmp(wifi_mode->valuestring, "now") == 0)
            wm = DEVICE_PROFILE_WIFI_MODE_NOW;
        else if (strcmp(wifi_mode->valuestring, "mesh") == 0)
            wm = DEVICE_PROFILE_WIFI_MODE_MESH;
    }

    uint8_t flags = 0;
    if (wifi_en && cJSON_IsTrue(wifi_en)) flags |= 0x01;
    if (eth_en && cJSON_IsTrue(eth_en)) flags |= 0x02;
    if (rs485_en && cJSON_IsTrue(rs485_en)) flags |= 0x04;

    bool ok = cluster_transport_send_remote_transport_set(
        node_id, p, f, wm, flags, node_identity_get(), 5000U);

    cJSON_Delete(root);

    if (ok) {
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, "{\"ok\":true}");
    } else {
        httpd_resp_set_status(req, "408 Request Timeout");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, "{\"error\":\"Timeout or failed\"}");
    }
    return ESP_OK;
}

static esp_err_t node_adopt_handler(httpd_req_t *req)
{
    uint32_t id = 0U;
    char audit_detail[64];

    if (!http_auth_require_cap(req, AUTH_CAP_NODE_ADMISSION))
        return ESP_OK;

    http_set_private_text_headers(req);
    http_set_cors_headers(req);

    if (!query_get_u32(req, "id", &id))
    {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_sendstr(req, "BAD_REQUEST");
        return ESP_OK;
    }

    if (node_registry_get_state(id) < NODE_REGISTRY_STATE_ACTIVE)
    {
        httpd_resp_set_status(req, "409 Conflict");
        httpd_resp_sendstr(req, "MUST_USE_REMOTE_CLAIM");
        return ESP_OK;
    }

    if (!node_registry_adopt(id))
    {
        httpd_resp_set_status(req, "404 Not Found");
        httpd_resp_sendstr(req, "NOT_FOUND");
        return ESP_OK;
    }

    snprintf(audit_detail, sizeof(audit_detail), "node=%" PRIu32, id);
    auth_audit_log("node_adopted", audit_detail);
    http_server_notify_state_change();
    httpd_resp_sendstr(req, "ADOPTED");
    return ESP_OK;
}

static esp_err_t nodes_template_apply_handler(httpd_req_t *req)
{
    char body[HTTP_BODY_BUFFER_SIZE] = {0};
    char node_id_text[24] = {0};
    char template_name[NODE_REGISTRY_TEMPLATE_LEN] = {0};
    char profile[NODE_REGISTRY_PROFILE_LEN] = {0};
    uint32_t node_id = 0U;

    if (!http_auth_require_cap(req, AUTH_CAP_NODE_ADMISSION))
        return ESP_OK;

    http_set_private_json_headers(req);

    if (!http_read_request_body(req, body, sizeof(body)) ||
        !http_body_get_value(body, "id", node_id_text, sizeof(node_id_text)) ||
        !http_body_get_value(body, "template", template_name, sizeof(template_name)))
    {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"bad_request\"}");
        return ESP_OK;
    }

    node_id = (uint32_t)strtoul(node_id_text, NULL, 10);
    if (node_id == 0U)
    {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"invalid_node\"}");
        return ESP_OK;
    }

    if (strcmp(template_name, "relay-node") == 0) {
        strcpy(profile, "relay-node");
        installation_map_upsert(node_id, 1, 0, "Q0", "", "Relé 1", "", "", 0, "", "");
        installation_map_upsert(node_id, 1, 1, "Q1", "", "Relé 2", "", "", 0, "", "");
    } else if (strcmp(template_name, "sensor-node") == 0) {
        strcpy(profile, "sensor-node");
        installation_map_upsert(node_id, 0, 0, "I0", "", "Sensor Principal", "", "", 0, "", "");
    } else {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"invalid_template\"}");
        return ESP_OK;
    }

    installation_map_save();
    
    if (!node_registry_configure(node_id, profile, template_name))
    {
        httpd_resp_set_status(req, "500 Internal Server Error");
        httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"registry_error\"}");
        return ESP_OK;
    }

    auth_audit_log("template_applied", template_name);

    httpd_resp_sendstr(req, "{\"ok\":true}");
    return ESP_OK;
}

static esp_err_t nodes_replace_handler(httpd_req_t *req)
{
    char body[HTTP_BODY_BUFFER_SIZE] = {0};
    char old_id_text[24] = {0};
    char new_id_text[24] = {0};
    uint32_t old_id = 0U;
    uint32_t new_id = 0U;

    if (!http_auth_require_cap(req, AUTH_CAP_NODE_ADMISSION))
        return ESP_OK;

    http_set_private_json_headers(req);

    if (!http_read_request_body(req, body, sizeof(body)) ||
        !http_body_get_value(body, "old_id", old_id_text, sizeof(old_id_text)) ||
        !http_body_get_value(body, "new_id", new_id_text, sizeof(new_id_text)))
    {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"bad_request\"}");
        return ESP_OK;
    }

    old_id = (uint32_t)strtoul(old_id_text, NULL, 10);
    new_id = (uint32_t)strtoul(new_id_text, NULL, 10);

    if (old_id == 0U || new_id == 0U || old_id == new_id)
    {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"invalid_nodes\"}");
        return ESP_OK;
    }

    if (!node_registry_replace(old_id, new_id))
    {
        httpd_resp_set_status(req, "500 Internal Server Error");
        httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"registry_error\"}");
        return ESP_OK;
    }

    installation_map_replace_node(old_id, new_id);

    auth_audit_log("node_replaced", new_id_text);

    httpd_resp_sendstr(req, "{\"ok\":true}");
    return ESP_OK;
}

static esp_err_t nodes_clone_handler(httpd_req_t *req)
{
    char body[HTTP_BODY_BUFFER_SIZE] = {0};
    char old_id_text[24] = {0};
    char new_id_text[24] = {0};
    uint32_t old_id = 0U;
    uint32_t new_id = 0U;

    if (!http_auth_require_cap(req, AUTH_CAP_NODE_ADMISSION))
        return ESP_OK;

    http_set_private_json_headers(req);

    if (!http_read_request_body(req, body, sizeof(body)) ||
        !http_body_get_value(body, "old_id", old_id_text, sizeof(old_id_text)) ||
        !http_body_get_value(body, "new_id", new_id_text, sizeof(new_id_text)))
    {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"bad_request\"}");
        return ESP_OK;
    }

    old_id = (uint32_t)strtoul(old_id_text, NULL, 10);
    new_id = (uint32_t)strtoul(new_id_text, NULL, 10);

    if (old_id == 0U || new_id == 0U || old_id == new_id)
    {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"invalid_nodes\"}");
        return ESP_OK;
    }

    if (!installation_map_clone_node(old_id, new_id))
    {
        httpd_resp_set_status(req, "500 Internal Server Error");
        httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"installation_map_error\"}");
        return ESP_OK;
    }

    auth_audit_log("node_cloned", new_id_text);

    httpd_resp_sendstr(req, "{\"ok\":true}");
    return ESP_OK;
}

static esp_err_t control_proxy_handler(httpd_req_t *req)
{
    uint32_t target_node_id = 0U;
    uint32_t self_id = node_identity_get();
    node_registry_entry_t entries_snap[NODE_REGISTRY_MAX_NODES];
    uint32_t target_ip = 0U;
    bool target_is_online = false;
    int count = 0;

    if (!http_auth_require_cap(req, AUTH_CAP_AUTOMATION_WRITE))
        return ESP_OK;

    http_set_private_text_headers(req);

    if (!query_get_u32(req, "target_node_id", &target_node_id) || target_node_id == 0U)
    {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"INVALID_TARGET_NODE_ID\"}");
        return ESP_OK;
    }

    if (target_node_id == self_id)
    {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"LOCAL_NODE_NO_PROXY\"}");
        return ESP_OK;
    }

    count = node_registry_export(entries_snap, NODE_REGISTRY_MAX_NODES);
    for (int i = 0; i < count; i++)
    {
        if (entries_snap[i].node_id == target_node_id)
        {
            target_ip = entries_snap[i].last_ip_addr;
            target_is_online = (entries_snap[i].cluster_state == CLUSTER_NODE_ONLINE &&
                                entries_snap[i].registry_state == NODE_REGISTRY_STATE_ACTIVE);
            break;
        }
    }

    if (target_ip == 0U || !target_is_online)
    {
        httpd_resp_set_status(req, "503 Service Unavailable");
        httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"TARGET_OFFLINE\"}");
        return ESP_OK;
    }

    /* Extract proxy path from URI query: e.g. /api/control/proxy?target_node_id=123&path=%2Fapi%2F... or /api/control/proxy?target_node_id=123&path=/api/... */
    const char *qmark = strchr(req->uri, '?');
    const char *path_param = qmark ? strstr(qmark, "path=") : NULL;
    if (!path_param)
    {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"MISSING_PROXY_PATH\"}");
        return ESP_OK;
    }

    path_param += 5; /* Skip "path=" */
    char forward_path[256];
    if (path_param[0] == '%' && path_param[1] == '2' && (path_param[2] == 'F' || path_param[2] == 'f'))
    {
        /* URL-encoded path: decode into forward_path */
        size_t out_idx = 0;
        for (size_t in_idx = 0; path_param[in_idx] != '\0' && out_idx < sizeof(forward_path) - 1; in_idx++)
        {
            if (path_param[in_idx] == '%' && path_param[in_idx + 1] != '\0' && path_param[in_idx + 2] != '\0')
            {
                char hex[3] = { path_param[in_idx + 1], path_param[in_idx + 2], '\0' };
                forward_path[out_idx++] = (char)strtol(hex, NULL, 16);
                in_idx += 2;
            }
            else
            {
                forward_path[out_idx++] = path_param[in_idx];
            }
        }
        forward_path[out_idx] = '\0';
    }
    else
    {
        strncpy(forward_path, path_param, sizeof(forward_path) - 1);
        forward_path[sizeof(forward_path) - 1] = '\0';
    }

    int sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (sock < 0)
    {
        httpd_resp_set_status(req, "500 Internal Server Error");
        httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"SOCKET_FAILED\"}");
        return ESP_OK;
    }

    struct timeval tv = { .tv_sec = 3, .tv_usec = 0 };
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    struct sockaddr_in dest_addr;
    memset(&dest_addr, 0, sizeof(dest_addr));
    dest_addr.sin_family = AF_INET;
    dest_addr.sin_port = htons(80);
    dest_addr.sin_addr.s_addr = target_ip;

    if (connect(sock, (struct sockaddr *)&dest_addr, sizeof(dest_addr)) != 0)
    {
        close(sock);
        httpd_resp_set_status(req, "503 Service Unavailable");
        httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"TARGET_UNREACHABLE\"}");
        return ESP_OK;
    }

    /* Build HTTP request to remote node */
    char req_buf[512];
    int req_len = snprintf(req_buf, sizeof(req_buf),
        "GET %s HTTP/1.1\r\n"
        "Host: %d.%d.%d.%d\r\n"
        "User-Agent: ENDAP-Gateway-ControlPlane\r\n"
        "Connection: close\r\n\r\n",
        forward_path,
        (int)(target_ip & 0xFF),
        (int)((target_ip >> 8) & 0xFF),
        (int)((target_ip >> 16) & 0xFF),
        (int)((target_ip >> 24) & 0xFF));

    if (send(sock, req_buf, req_len, 0) < 0)
    {
        close(sock);
        httpd_resp_set_status(req, "500 Internal Server Error");
        httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"PROXY_SEND_FAILED\"}");
        return ESP_OK;
    }

    /* Read response from remote node and stream to client */
    char chunk[512];
    int chunk_len = 0;
    bool headers_parsed = false;
    char header_buf[1024];
    int header_len = 0;

    while ((chunk_len = recv(sock, chunk, sizeof(chunk), 0)) > 0)
    {
        if (!headers_parsed)
        {
            int to_copy = chunk_len;
            if (header_len + to_copy > (int)sizeof(header_buf) - 1)
                to_copy = (int)sizeof(header_buf) - 1 - header_len;

            memcpy(header_buf + header_len, chunk, (size_t)to_copy);
            header_len += to_copy;
            header_buf[header_len] = '\0';

            char *hdr_end = strstr(header_buf, "\r\n\r\n");
            if (hdr_end)
            {
                headers_parsed = true;
                int http_status = 200;
                if (sscanf(header_buf, "HTTP/1.%*d %d", &http_status) != 1)
                {
                    http_status = 200;
                }

                if (http_status == 409)
                    httpd_resp_set_status(req, "409 Conflict");
                else if (http_status == 404)
                    httpd_resp_set_status(req, "404 Not Found");
                else if (http_status == 503)
                    httpd_resp_set_status(req, "503 Service Unavailable");
                else if (http_status >= 400)
                {
                    char status_hdr[32];
                    snprintf(status_hdr, sizeof(status_hdr), "%d Error", http_status);
                    httpd_resp_set_status(req, status_hdr);
                }
                else
                {
                    httpd_resp_set_status(req, "200 OK");
                }

                /* Calculate offset where body starts in this chunk */
                int hdr_bytes_in_buf = (int)(hdr_end + 4 - header_buf);
                char *chunk_hdr_end = strstr(chunk, "\r\n\r\n");
                if (chunk_hdr_end)
                {
                    char *body_start = chunk_hdr_end + 4;
                    int body_bytes = chunk_len - (int)(body_start - chunk);
                    if (body_bytes > 0)
                    {
                        httpd_resp_send_chunk(req, body_start, (size_t)body_bytes);
                    }
                }
                else
                {
                    /* Header was split across chunks */
                    int body_bytes = header_len - hdr_bytes_in_buf;
                    if (body_bytes > 0)
                    {
                        httpd_resp_send_chunk(req, hdr_end + 4, (size_t)body_bytes);
                    }
                }
            }
        }
        else
        {
            httpd_resp_send_chunk(req, chunk, (size_t)chunk_len);
        }
    }

    close(sock);

    if (!headers_parsed)
    {
        httpd_resp_set_status(req, "504 Gateway Timeout");
        httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"REMOTE_NO_RESPONSE\"}");
        return ESP_OK;
    }

    /* Finalize chunked response */
    httpd_resp_send_chunk(req, NULL, 0);
    return ESP_OK;
}

static esp_err_t node_configure_handler(httpd_req_t *req)
{
    uint32_t id = 0U;
    char profile[NODE_REGISTRY_PROFILE_LEN] = {0};
    char template_name[NODE_REGISTRY_TEMPLATE_LEN] = {0};
    char audit_detail[96];

    if (!http_auth_require_cap(req, AUTH_CAP_NODE_ADMISSION))
        return ESP_OK;

    http_set_private_text_headers(req);
    http_set_cors_headers(req);

    if (!query_get_u32(req, "id", &id) ||
        !query_get_str(req, "profile", profile, sizeof(profile)) ||
        !query_get_optional_str(req, "template", template_name, sizeof(template_name), NULL))
    {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_sendstr(req, "BAD_REQUEST");
        return ESP_OK;
    }

    if (node_registry_get_state(id) < NODE_REGISTRY_STATE_ACTIVE)
    {
        httpd_resp_set_status(req, "409 Conflict");
        httpd_resp_sendstr(req, "MUST_USE_REMOTE_CLAIM");
        return ESP_OK;
    }

    if (!node_registry_configure(id, profile, template_name))
    {
        httpd_resp_set_status(req, "404 Not Found");
        httpd_resp_sendstr(req, "NOT_FOUND");
        return ESP_OK;
    }

    snprintf(audit_detail,
             sizeof(audit_detail),
             "node=%" PRIu32 " profile=%s template=%s",
             id,
             profile,
             template_name[0] ? template_name : "-");
    auth_audit_log("node_configured", audit_detail);
    http_server_notify_state_change();
    httpd_resp_sendstr(req, "CONFIGURED");
    return ESP_OK;
}

static esp_err_t node_activate_handler(httpd_req_t *req)
{
    uint32_t id = 0U;
    char audit_detail[64];

    if (!http_auth_require_cap(req, AUTH_CAP_NODE_ADMISSION))
        return ESP_OK;

    http_set_private_text_headers(req);

    if (!query_get_u32(req, "id", &id))
    {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_sendstr(req, "BAD_REQUEST");
        return ESP_OK;
    }

    if (node_registry_get_state(id) < NODE_REGISTRY_STATE_ACTIVE)
    {
        httpd_resp_set_status(req, "409 Conflict");
        httpd_resp_sendstr(req, "MUST_USE_REMOTE_CLAIM");
        return ESP_OK;
    }

    if (!node_registry_activate(id))
    {
        httpd_resp_set_status(req, "409 Conflict");
        httpd_resp_sendstr(req, "INVALID_STATE");
        return ESP_OK;
    }

    snprintf(audit_detail, sizeof(audit_detail), "node=%" PRIu32, id);
    auth_audit_log("node_activated", audit_detail);
    http_server_notify_state_change();
    httpd_resp_sendstr(req, "ACTIVATED");
    return ESP_OK;
}

static esp_err_t node_revoke_handler(httpd_req_t *req)
{
    uint32_t id = 0U;
    char audit_detail[64];

    if (!http_auth_require_cap(req, AUTH_CAP_NODE_ADMISSION))
        return ESP_OK;

    http_set_private_text_headers(req);

    if (!query_get_u32(req, "id", &id))
    {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_sendstr(req, "BAD_REQUEST");
        return ESP_OK;
    }

    if (!node_registry_revoke(id))
    {
        httpd_resp_set_status(req, "404 Not Found");
        httpd_resp_sendstr(req, "NOT_FOUND");
        return ESP_OK;
    }

    snprintf(audit_detail, sizeof(audit_detail), "node=%" PRIu32, id);
    auth_audit_log("node_revoked", audit_detail);
    http_server_notify_state_change();
    httpd_resp_sendstr(req, "REVOKED");
    return ESP_OK;
}

static esp_err_t kernel_load_handler(httpd_req_t *req)
{
    char action[16] = {0};
    char phase_text[24] = {0};
    int extra_us = 0;
    uint8_t phase = 0U;
    char audit_detail[96];

    if (!http_auth_require_cap(req, AUTH_CAP_RUNTIME_DIAGNOSTICS))
        return ESP_OK;

    http_set_private_text_headers(req);

    query_get_optional_str(req, "action", action, sizeof(action), NULL);

    if (action[0] != '\0' &&
        strcmp(action, "clear") == 0)
    {
        phase_load_test_clear();
        auth_audit_log("kernel_load", "clear");
        http_server_notify_state_change();
        httpd_resp_sendstr(req, "CLEARED");
        return ESP_OK;
    }

    if (!query_get_str(req, "phase", phase_text, sizeof(phase_text)) ||
        !kernel_load_phase_from_text(phase_text, &phase) ||
        !query_get_int(req, "us", 0, INT_MAX, &extra_us))
    {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_sendstr(req, "BAD_REQUEST");
        return ESP_OK;
    }

    if (extra_us < 0)
        extra_us = 0;

    /* v1 simplification: keep only one active validation load at a time */
    phase_load_test_clear();

    if (extra_us > 0)
        phase_load_test_set(phase, (uint32_t)extra_us);

    snprintf(audit_detail, sizeof(audit_detail), "phase=%s us=%d", phase_text, extra_us);
    auth_audit_log("kernel_load", audit_detail);
    http_server_notify_state_change();
    httpd_resp_sendstr(req, (extra_us == 0) ? "CLEARED" : "APPLIED");
    return ESP_OK;
}

static esp_err_t automation_list_handler(httpd_req_t *req)
{
    if (!http_auth_require_cap(req, AUTH_CAP_DASHBOARD_READ))
        return ESP_OK;

    char *json_buf = json_buffer_malloc(AUTOMATION_JSON_BUFFER_SIZE);
    if (!json_buf) { json_buffer_free(NULL); httpd_resp_send_500(req); return ESP_OK; }

    size_t len = build_automation_json(json_buf, AUTOMATION_JSON_BUFFER_SIZE);

    http_set_private_json_headers(req);

    if (len == 0U)
        httpd_resp_send(req, "{\"saved\":0,\"count\":0,\"rules\":[]}", HTTPD_RESP_USE_STRLEN);
    else
        httpd_resp_send(req, json_buf, len);

    json_buffer_free(json_buf);

    return ESP_OK;
}


static esp_err_t automation_ladder_handler(httpd_req_t *req)
{
    if (req->method == HTTP_GET) {
        http_set_public_json_headers(req);
        uint8_t program_buf[2048];
        size_t len = ladder_engine_get_program(program_buf, sizeof(program_buf));
        if (len == 0) {
            httpd_resp_set_status(req, "404 Not Found");
            httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"no_program_loaded\"}");
            return ESP_OK;
        }
        httpd_resp_set_type(req, "application/octet-stream");
        httpd_resp_send(req, (const char *)program_buf, len);
        return ESP_OK;
    }

    if (!http_auth_require_cap(req, AUTH_CAP_AUTOMATION_WRITE))
        return ESP_OK;

    http_set_public_json_headers(req);

    if (req->content_len == 0) {
        ladder_engine_clear();
        httpd_resp_sendstr(req, "{\"ok\":true,\"status\":\"ladder_cleared\"}");
        return ESP_OK;
    }

    if (req->content_len > 2048) {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"invalid_payload_size\"}");
        return ESP_OK;
    }

    uint8_t *buf = malloc(req->content_len);
    if (!buf) {
        httpd_resp_set_status(req, "500 Internal Server Error");
        httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"no_memory\"}");
        return ESP_OK;
    }

    int ret = httpd_req_recv(req, (char *)buf, req->content_len);
    if (ret <= 0) {
        free(buf);
        httpd_resp_set_status(req, "500 Internal Server Error");
        httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"recv_failed\"}");
        return ESP_OK;
    }

    esp_err_t err = ladder_engine_load_program(buf, req->content_len);
    free(buf);

    if (err != ESP_OK) {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"invalid_bytecode\"}");
        return ESP_OK;
    }

    httpd_resp_sendstr(req, "{\"ok\":true,\"status\":\"ladder_deployed\"}");
    return ESP_OK;
}

static esp_err_t automation_add_handler(httpd_req_t *req)
{
    int input = 0;
    int threshold = 0;
    int output = 0;
    int duration_ms = 0;
    int on_true = 1;
    int on_false = 0;
    uint8_t op = AUTOMATION_OP_GT;
    uint8_t mode = AUTOMATION_MODE_FOLLOW;
    char op_text[8];
    char mode_text[16];
    char audit_detail[96];
    automation_result_t result;

    if (!http_auth_require_cap(req, AUTH_CAP_AUTOMATION_WRITE))
        return ESP_OK;

    http_set_private_text_headers(req);

    int input_gpio = -1;
    bool has_input_gpio = false;
    int output_gpio = -1;
    bool has_output_gpio = false;

    query_get_optional_int(req, "input_gpio", 0, 39, &input_gpio, &has_input_gpio);
    query_get_optional_int(req, "output_gpio", 0, 39, &output_gpio, &has_output_gpio);

    if (has_input_gpio)
    {
        io_binding_input_view_t in_views[IO_BINDING_MAX_INPUTS];
        int in_count = io_binding_export_inputs(in_views, IO_BINDING_MAX_INPUTS);
        bool found_in = false;
        for (int i = 0; i < in_count; i++)
        {
            if (in_views[i].gpio == input_gpio)
            {
                input = in_views[i].id;
                found_in = true;
                break;
            }
        }
        if (!found_in)
        {
            io_binding_output_view_t out_views[IO_BINDING_MAX_OUTPUTS];
            int out_count = io_binding_export_outputs(out_views, IO_BINDING_MAX_OUTPUTS);
            for (int i = 0; i < out_count; i++)
            {
                if (out_views[i].gpio == input_gpio)
                {
                    input = out_views[i].id;
                    found_in = true;
                    break;
                }
            }
        }
        if (!found_in)
        {
            httpd_resp_set_status(req, "400 Bad Request");
            httpd_resp_sendstr(req, "INPUT_GPIO_NOT_BOUND");
            return ESP_OK;
        }
    }
    else if (!query_get_int(req, "input", 0, UINT16_MAX, &input))
    {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_sendstr(req, "BAD_REQUEST");
        return ESP_OK;
    }

    if (!query_get_int(req, "threshold", INT_MIN, INT_MAX, &threshold))
    {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_sendstr(req, "BAD_REQUEST");
        return ESP_OK;
    }

    if (has_output_gpio)
    {
        io_binding_output_view_t out_views[IO_BINDING_MAX_OUTPUTS];
        int out_count = io_binding_export_outputs(out_views, IO_BINDING_MAX_OUTPUTS);
        bool found_out = false;
        for (int i = 0; i < out_count; i++)
        {
            if (out_views[i].gpio == output_gpio)
            {
                output = out_views[i].id;
                found_out = true;
                break;
            }
        }
        if (!found_out)
        {
            httpd_resp_set_status(req, "400 Bad Request");
            httpd_resp_sendstr(req, "OUTPUT_GPIO_NOT_BOUND");
            return ESP_OK;
        }
    }
    else if (!query_get_int(req, "output", 0, UINT16_MAX, &output))
    {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_sendstr(req, "BAD_REQUEST");
        return ESP_OK;
    }

    if (!query_get_optional_str(req, "op", op_text, sizeof(op_text), NULL))
    {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_sendstr(req, "BAD_REQUEST");
        return ESP_OK;
    }

    if (op_text[0] != '\0' &&
        !automation_engine_operator_from_code(op_text, &op))
    {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_sendstr(req, "INVALID_OPERATOR");
        return ESP_OK;
    }

    if (!query_get_optional_str(req, "mode", mode_text, sizeof(mode_text), NULL))
    {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_sendstr(req, "BAD_REQUEST");
        return ESP_OK;
    }

    if (mode_text[0] != '\0' &&
        !automation_engine_mode_from_code(mode_text, &mode))
    {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_sendstr(req, "INVALID_MODE");
        return ESP_OK;
    }

    if (!query_get_optional_int(req, "duration_ms", 0, UINT16_MAX, &duration_ms, NULL) ||
        !query_get_optional_int(req, "on_true", 0, 1, &on_true, NULL) ||
        !query_get_optional_int(req, "on_false", 0, 1, &on_false, NULL))
    {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_sendstr(req, "BAD_REQUEST");
        return ESP_OK;
    }

    result = automation_engine_add_rule(
        input,
        op,
        mode,
        threshold,
        (duration_ms < 0) ? 0U : (uint16_t)duration_ms,
        output,
        on_true,
        on_false);

    if (result == AUTOMATION_RESULT_INVALID)
    {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_sendstr(req, "INVALID_RULE");
        return ESP_OK;
    }

    if (result == AUTOMATION_RESULT_CONFLICT)
    {
        httpd_resp_set_status(req, "409 Conflict");
        httpd_resp_sendstr(req, "OUTPUT_CONFLICT");
        return ESP_OK;
    }

    if (result == AUTOMATION_RESULT_FULL)
    {
        httpd_resp_set_status(req, "409 Conflict");
        httpd_resp_sendstr(req, "RULE_TABLE_FULL");
        return ESP_OK;
    }

    int interlock_en = 0;
    query_get_optional_int(req, "interlock_en", 0, 1, &interlock_en, NULL);
    if (interlock_en)
    {
        automation_interlock_t ilk = {0};
        ilk.enabled = 1;
        int logic_op = 0;
        query_get_optional_int(req, "interlock_logic", 0, 1, &logic_op, NULL);
        ilk.logic_op = (uint8_t)logic_op;

        int cond1_chan = -1, cond1_val = 0;
        char cond1_op_text[8] = {0};
        query_get_optional_int(req, "interlock_cond1_channel", 0, UINT16_MAX, &cond1_chan, NULL);
        query_get_optional_str(req, "interlock_cond1_op", cond1_op_text, sizeof(cond1_op_text), NULL);
        query_get_optional_int(req, "interlock_cond1_val", -128, 127, &cond1_val, NULL);

        if (cond1_chan >= 0)
        {
            uint8_t op1 = AUTOMATION_OP_EQ;
            if (cond1_op_text[0] != '\0')
                automation_engine_operator_from_code(cond1_op_text, &op1);
            ilk.conditions[ilk.cond_count].channel = (uint16_t)cond1_chan;
            ilk.conditions[ilk.cond_count].op = op1;
            ilk.conditions[ilk.cond_count].target_val = (int8_t)cond1_val;
            ilk.cond_count++;
        }

        int cond2_chan = -1, cond2_val = 0;
        char cond2_op_text[8] = {0};
        query_get_optional_int(req, "interlock_cond2_channel", 0, UINT16_MAX, &cond2_chan, NULL);
        query_get_optional_str(req, "interlock_cond2_op", cond2_op_text, sizeof(cond2_op_text), NULL);
        query_get_optional_int(req, "interlock_cond2_val", -128, 127, &cond2_val, NULL);

        if (cond2_chan >= 0)
        {
            uint8_t op2 = AUTOMATION_OP_EQ;
            if (cond2_op_text[0] != '\0')
                automation_engine_operator_from_code(cond2_op_text, &op2);
            ilk.conditions[ilk.cond_count].channel = (uint16_t)cond2_chan;
            ilk.conditions[ilk.cond_count].op = op2;
            ilk.conditions[ilk.cond_count].target_val = (int8_t)cond2_val;
            ilk.cond_count++;
        }

        int count = automation_engine_get_node_count();
        if (count > 0)
            automation_engine_set_interlock_at(count - 1, &ilk);
    }

    snprintf(audit_detail,
             sizeof(audit_detail),
             "input=%d output=%d mode=%s interlock=%d",
             input,
             output,
             mode_text[0] ? mode_text : automation_engine_mode_to_code(mode),
             interlock_en);
    auth_audit_log("automation_added", audit_detail);
    http_server_notify_state_change();
    httpd_resp_sendstr(req, (result == AUTOMATION_RESULT_DUPLICATE) ? "DUPLICATE" : "OK");
    return ESP_OK;
}

static esp_err_t automation_remove_handler(httpd_req_t *req)
{
    int index = -1;

    if (!http_auth_require_cap(req, AUTH_CAP_AUTOMATION_WRITE))
        return ESP_OK;

    http_set_private_text_headers(req);

    if (!query_get_int(req, "index", 0, AUTOMATION_ENGINE_MAX_NODES - 1, &index))
    {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_sendstr(req, "BAD_REQUEST");
        return ESP_OK;
    }

    if (!automation_engine_remove_node_at(index))
    {
        httpd_resp_set_status(req, "404 Not Found");
        httpd_resp_sendstr(req, "NOT_FOUND");
        return ESP_OK;
    }

    auth_audit_log("automation_removed", "rule-removed");
    http_server_notify_state_change();
    httpd_resp_sendstr(req, "REMOVED");
    return ESP_OK;
}

static esp_err_t automation_clear_handler(httpd_req_t *req)
{
    if (!http_auth_require_cap(req, AUTH_CAP_AUTOMATION_WRITE))
        return ESP_OK;

    http_set_private_text_headers(req);

    automation_engine_clear();
    ladder_engine_clear();
    auth_audit_log("automation_cleared", "all-rules-cleared");
    http_server_notify_state_change();
    httpd_resp_sendstr(req, "CLEARED");
    return ESP_OK;
}

/* ============================================================
   START SERVER (🔥 FINAL)
============================================================ */

extern void core_mem_audit(const char *phase);

void http_server_start(void)
{
    if (json_api_mutex == NULL) {
        json_api_mutex = xSemaphoreCreateMutex();
    }
    core_mem_audit("HTTP_PRE_BUFFERS");

    // Buffer allocations removed to save permanent heap space
    if (!automation_rules_snapshot) automation_rules_snapshot = malloc(sizeof(automation_node_t) * AUTOMATION_ENGINE_MAX_NODES);
    if (!automation_diag_snapshot) automation_diag_snapshot = malloc(sizeof(automation_rule_diag_t) * AUTOMATION_ENGINE_MAX_NODES);
    if (!input_profile_snapshot) input_profile_snapshot = malloc(sizeof(io_binding_input_view_t) * IO_BINDING_MAX_INPUTS);
    if (!output_profile_snapshot) output_profile_snapshot = malloc(sizeof(io_binding_output_view_t) * IO_BINDING_MAX_OUTPUTS);
    if (!status_output_snapshot) status_output_snapshot = malloc(sizeof(io_binding_output_view_t) * IO_BINDING_MAX_OUTPUTS);
    if (!status_input_diag_snapshot) status_input_diag_snapshot = malloc(sizeof(io_driver_input_diag_t) * STATUS_IO_MAX_CHANNELS);
    if (!failsafe_status_snapshot) failsafe_status_snapshot = malloc(sizeof(failsafe_output_status_t) * FAILSAFE_MAX_OUTPUTS);

    core_mem_audit("HTTP_POST_BUFFERS");

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    /* O projeto usa um teto global enxuto de sockets no lwIP.
       O default do esp_http_server (7 clientes + 3 internos) consome
       sozinho todo o orcamento de CONFIG_LWIP_MAX_SOCKETS=10, deixando
       cluster/DNS/rede sem margem e provocando accept() -> ENFILE (23).
       Para a dashboard embarcada V1, 3 sessoes simultaneas bastam:
       1 WebSocket + 1/2 requests HTTP concorrentes do operador. */
    config.max_open_sockets = 3;
    config.max_uri_handlers = HTTPD_MAX_URI_HANDLERS;
    config.stack_size = HTTPD_STACK_SIZE;
    config.lru_purge_enable = true;
    config.recv_wait_timeout = 2;
    config.send_wait_timeout = 2;

    /* Aba fechada/desconexao de WS eh ruido normal de browser. */
    esp_log_level_set("httpd_ws", ESP_LOG_ERROR);
    /* Keep-alive encerrado pelo browser durante fetch/polling nao deve poluir o serial. */
    esp_log_level_set("httpd_txrx", ESP_LOG_ERROR);

    ws_count = 0;
    ws_broadcast_pending = false;
    ws_last_periodic_us = (uint64_t)esp_timer_get_time();
    auth_init();
    installation_map_load();

    if (httpd_start(&server, &config) == ESP_OK)
    {
        /* ROOT */
        httpd_register_uri_handler(server, &(httpd_uri_t){
            .uri = "/", .method = HTTP_GET, .handler = redirect_to_dash });

        /* Preflight PNA/CORS: responde OPTIONS em qualquer rota com 204 +
           headers de cors, incluindo Access-Control-Allow-Private-Network.
           Cobre todas as chamadas do Studio (não é preciso registrar
           OPTIONS por URI). */
        httpd_register_err_handler(server, HTTPD_404_NOT_FOUND, options_error_handler);
        httpd_register_err_handler(server, HTTPD_405_METHOD_NOT_ALLOWED, options_error_handler);

        /* AUTH */
        httpd_register_uri_handler(server, &(httpd_uri_t){
            .uri = "/api/auth/login", .method = HTTP_POST, .handler = auth_login_handler });

        httpd_register_uri_handler(server, &(httpd_uri_t){
            .uri = "/api/auth/logout", .method = HTTP_POST, .handler = auth_logout_handler });

        httpd_register_uri_handler(server, &(httpd_uri_t){
            .uri = "/api/auth/status", .method = HTTP_GET, .handler = auth_status_handler });

        httpd_register_uri_handler(server, &(httpd_uri_t){
            .uri = "/api/auth/bootstrap", .method = HTTP_POST, .handler = auth_bootstrap_handler });

        httpd_register_uri_handler(server, &(httpd_uri_t){
            .uri = "/api/auth/password", .method = HTTP_POST, .handler = auth_password_handler });

        httpd_register_uri_handler(server, &(httpd_uri_t){
            .uri = "/api/auth/users", .method = HTTP_GET, .handler = auth_users_handler });

        httpd_register_uri_handler(server, &(httpd_uri_t){
            .uri = "/api/auth/audit", .method = HTTP_GET, .handler = auth_audit_handler });

        httpd_register_uri_handler(server, &(httpd_uri_t){
            .uri = "/api/auth/users/save", .method = HTTP_POST, .handler = auth_users_save_handler });

        httpd_register_uri_handler(server, &(httpd_uri_t){
            .uri = "/api/auth/users/delete", .method = HTTP_POST, .handler = auth_users_delete_handler });

        /* API IO */
        httpd_register_uri_handler(server, &(httpd_uri_t){
            .uri = "/api/resources", .method = HTTP_GET, .handler = resources_handler });

        httpd_register_uri_handler(server, &(httpd_uri_t){
            .uri = "/api/set", .method = HTTP_GET, .handler = set_handler });
        httpd_register_uri_handler(server, &(httpd_uri_t){
            .uri = "/api/set", .method = HTTP_OPTIONS, .handler = options_handler });

        httpd_register_uri_handler(server, &(httpd_uri_t){
            .uri = "/api/resource/actuate", .method = HTTP_POST, .handler = resource_actuate_handler });


        httpd_register_uri_handler(server, &(httpd_uri_t){
            .uri = "/api/status", .method = HTTP_GET, .handler = status_handler });

        httpd_register_uri_handler(server, &(httpd_uri_t){
            .uri = "/api/public/status", .method = HTTP_GET, .handler = public_status_handler });
        httpd_register_uri_handler(server, &(httpd_uri_t){
            .uri = "/api/public/status", .method = HTTP_OPTIONS, .handler = options_handler });

        httpd_register_uri_handler(server, &(httpd_uri_t){
            .uri = "/api/failsafe", .method = HTTP_GET, .handler = failsafe_handler });

        httpd_register_uri_handler(server, &(httpd_uri_t){
            .uri = "/api/failsafe/status", .method = HTTP_GET, .handler = failsafe_handler });

        httpd_register_uri_handler(server, &(httpd_uri_t){
            .uri = "/api/failsafe/outputs", .method = HTTP_GET, .handler = failsafe_handler });

        httpd_register_uri_handler(server, &(httpd_uri_t){
            .uri = "/api/failsafe/save", .method = HTTP_POST, .handler = failsafe_save_handler });

        httpd_register_uri_handler(server, &(httpd_uri_t){
            .uri = "/api/failsafe/output/save", .method = HTTP_POST, .handler = failsafe_save_handler });

        httpd_register_uri_handler(server, &(httpd_uri_t){
            .uri = "/api/failsafe/rearm", .method = HTTP_POST, .handler = failsafe_rearm_handler });

        httpd_register_uri_handler(server, &(httpd_uri_t){
            .uri = "/api/failsafe/output/reset", .method = HTTP_POST, .handler = failsafe_rearm_handler });

        httpd_register_uri_handler(server, &(httpd_uri_t){
            .uri = "/api/failsafe/output/test", .method = HTTP_POST, .handler = failsafe_test_handler });

        httpd_register_uri_handler(server, &(httpd_uri_t){
            .uri = "/api/public/profile", .method = HTTP_GET, .handler = public_profile_handler });

        httpd_register_uri_handler(server, &(httpd_uri_t){
            .uri = "/api/profile", .method = HTTP_GET, .handler = profile_handler });

        httpd_register_uri_handler(server, &(httpd_uri_t){
            .uri = "/api/installation/channel-map", .method = HTTP_GET, .handler = installation_map_handler });

        httpd_register_uri_handler(server, &(httpd_uri_t){
            .uri = "/api/installation/channel-map/save", .method = HTTP_POST, .handler = installation_map_save_handler });

        httpd_register_uri_handler(server, &(httpd_uri_t){
            .uri = "/api/io/map", .method = HTTP_GET, .handler = io_map_handler });
        httpd_register_uri_handler(server, &(httpd_uri_t){
            .uri = "/api/io/map", .method = HTTP_OPTIONS, .handler = options_handler });

        httpd_register_uri_handler(server, &(httpd_uri_t){
            .uri = "/api/pve", .method = HTTP_GET, .handler = pve_handler });

        httpd_register_uri_handler(server, &(httpd_uri_t){
            .uri = "/api/pve/config", .method = HTTP_POST, .handler = pve_save_config_handler });

        httpd_register_uri_handler(server, &(httpd_uri_t){
            .uri = "/api/pve/alarms/history", .method = HTTP_GET, .handler = pve_alarms_history_handler });

        httpd_register_uri_handler(server, &(httpd_uri_t){
            .uri = "/api/network/ethernet/ip", .method = HTTP_GET, .handler = ethernet_ip_config_get_handler });

        httpd_register_uri_handler(server, &(httpd_uri_t){
            .uri = "/api/network/ethernet/ip", .method = HTTP_POST, .handler = ethernet_ip_config_post_handler });



        httpd_register_uri_handler(server, &(httpd_uri_t){
            .uri = "/api/sensors/config", .method = HTTP_GET, .handler = sensors_config_get_handler });

        httpd_register_uri_handler(server, &(httpd_uri_t){
            .uri = "/api/sensors/config", .method = HTTP_POST, .handler = sensors_config_post_handler });

        httpd_register_uri_handler(server, &(httpd_uri_t){
            .uri = "/api/network/config", .method = HTTP_GET, .handler = network_config_handler });

        httpd_register_uri_handler(server, &(httpd_uri_t){
            .uri = "/api/network/preview", .method = HTTP_GET, .handler = network_preview_handler });

        httpd_register_uri_handler(server, &(httpd_uri_t){
            .uri = "/api/hardware/gpios", .method = HTTP_GET, .handler = hardware_gpios_handler });

        httpd_register_uri_handler(server, &(httpd_uri_t){
            .uri = "/api/i2c/scan", .method = HTTP_GET, .handler = i2c_scan_handler });

        httpd_register_uri_handler(server, &(httpd_uri_t){
            .uri = "/api/reboot", .method = HTTP_GET, .handler = reboot_handler });

        httpd_register_uri_handler(server, &(httpd_uri_t){
            .uri = "/api/recovery", .method = HTTP_GET, .handler = recovery_handler });

        httpd_register_uri_handler(server, &(httpd_uri_t){
            .uri = "/api/nodes", .method = HTTP_GET, .handler = nodes_handler });
        httpd_register_uri_handler(server, &(httpd_uri_t){
            .uri = "/api/nodes", .method = HTTP_OPTIONS, .handler = options_handler });

        httpd_register_uri_handler(server, &(httpd_uri_t){
            .uri = "/api/cluster/status", .method = HTTP_GET, .handler = cluster_status_handler });

        httpd_register_uri_handler(server, &(httpd_uri_t){
            .uri = "/api/onboarding/status", .method = HTTP_GET, .handler = onboarding_status_handler });
        httpd_register_uri_handler(server, &(httpd_uri_t){
            .uri = "/api/onboarding/status", .method = HTTP_OPTIONS, .handler = options_handler });

        httpd_register_uri_handler(server, &(httpd_uri_t){
            .uri = "/api/network/metrics", .method = HTTP_GET, .handler = network_metrics_handler });

        httpd_register_uri_handler(server, &(httpd_uri_t){
            .uri = "/api/onboarding/claim", .method = HTTP_POST, .handler = onboarding_claim_handler });
        httpd_register_uri_handler(server, &(httpd_uri_t){
            .uri = "/api/onboarding/claim", .method = HTTP_OPTIONS, .handler = options_handler });

        httpd_register_uri_handler(server, &(httpd_uri_t){
            .uri = "/api/onboarding/reset", .method = HTTP_POST, .handler = onboarding_reset_handler });
        httpd_register_uri_handler(server, &(httpd_uri_t){
            .uri = "/api/onboarding/reset", .method = HTTP_OPTIONS, .handler = options_handler });

        httpd_register_uri_handler(server, &(httpd_uri_t){
            .uri = "/api/system/factory-reset", .method = HTTP_POST, .handler = system_factory_reset_handler });

        httpd_register_uri_handler(server, &(httpd_uri_t){
            .uri = "/api/nodes/adopt", .method = HTTP_GET, .handler = node_adopt_handler });
        httpd_register_uri_handler(server, &(httpd_uri_t){
            .uri = "/api/nodes/adopt", .method = HTTP_OPTIONS, .handler = options_handler });

        httpd_register_uri_handler(server, &(httpd_uri_t){
            .uri = "/api/nodes/configure", .method = HTTP_GET, .handler = node_configure_handler });
        httpd_register_uri_handler(server, &(httpd_uri_t){
            .uri = "/api/nodes/configure", .method = HTTP_OPTIONS, .handler = options_handler });

        httpd_register_uri_handler(server, &(httpd_uri_t){
            .uri = "/api/nodes/template/apply", .method = HTTP_POST, .handler = nodes_template_apply_handler });

        httpd_register_uri_handler(server, &(httpd_uri_t){
            .uri = "/api/nodes/replace", .method = HTTP_POST, .handler = nodes_replace_handler });

        httpd_register_uri_handler(server, &(httpd_uri_t){
            .uri = "/api/nodes/clone", .method = HTTP_POST, .handler = nodes_clone_handler });

        httpd_register_uri_handler(server, &(httpd_uri_t){
            .uri = "/api/nodes/activate", .method = HTTP_GET, .handler = node_activate_handler });

        httpd_register_uri_handler(server, &(httpd_uri_t){
            .uri = "/api/nodes/revoke", .method = HTTP_GET, .handler = node_revoke_handler });

        httpd_register_uri_handler(server, &(httpd_uri_t){
            .uri = "/api/nodes/*/transport", .method = HTTP_POST, .handler = nodes_transport_handler });

        httpd_register_uri_handler(server, &(httpd_uri_t){
            .uri = "/api/control/proxy", .method = HTTP_GET, .handler = control_proxy_handler });

        httpd_register_uri_handler(server, &(httpd_uri_t){
            .uri = "/api/input/learn", .method = HTTP_GET, .handler = input_learn_handler });

        httpd_register_uri_handler(server, &(httpd_uri_t){
            .uri = "/api/input/config", .method = HTTP_GET, .handler = input_config_handler });

        httpd_register_uri_handler(server, &(httpd_uri_t){
            .uri = "/api/input/add", .method = HTTP_GET, .handler = input_add_handler });

        httpd_register_uri_handler(server, &(httpd_uri_t){
            .uri = "/api/input/remove", .method = HTTP_GET, .handler = input_remove_handler });

        httpd_register_uri_handler(server, &(httpd_uri_t){
            .uri = "/api/input/reset", .method = HTTP_GET, .handler = input_reset_handler });

        httpd_register_uri_handler(server, &(httpd_uri_t){
            .uri = "/api/output/config", .method = HTTP_GET, .handler = output_config_handler });

        httpd_register_uri_handler(server, &(httpd_uri_t){
            .uri = "/api/output/add", .method = HTTP_GET, .handler = output_add_handler });

        httpd_register_uri_handler(server, &(httpd_uri_t){
            .uri = "/api/output/remove", .method = HTTP_GET, .handler = output_remove_handler });

        httpd_register_uri_handler(server, &(httpd_uri_t){
            .uri = "/api/output/reset", .method = HTTP_GET, .handler = output_reset_handler });

        httpd_register_uri_handler(server, &(httpd_uri_t){
            .uri = "/api/automation", .method = HTTP_GET, .handler = automation_list_handler });

        httpd_register_uri_handler(server, &(httpd_uri_t){
            .uri = "/api/automation/add", .method = HTTP_GET, .handler = automation_add_handler });
        httpd_register_uri_handler(server, &(httpd_uri_t){
            .uri = "/api/automation/ladder", .method = HTTP_GET, .handler = automation_ladder_handler });
        httpd_register_uri_handler(server, &(httpd_uri_t){
            .uri = "/api/automation/ladder", .method = HTTP_POST, .handler = automation_ladder_handler });
        httpd_register_uri_handler(server, &(httpd_uri_t){
            .uri = "/api/automation/ladder", .method = HTTP_OPTIONS, .handler = options_handler });


        httpd_register_uri_handler(server, &(httpd_uri_t){
            .uri = "/api/automation/remove", .method = HTTP_GET, .handler = automation_remove_handler });

        httpd_register_uri_handler(server, &(httpd_uri_t){
            .uri = "/api/automation/clear", .method = HTTP_GET, .handler = automation_clear_handler });

#ifdef CONFIG_ENDAP_ENABLE_CHAOS_TESTING
        httpd_register_uri_handler(server, &(httpd_uri_t){
            .uri = "/api/chaos/inject_delay", .method = HTTP_POST, .handler = chaos_inject_delay_handler });

        httpd_register_uri_handler(server, &(httpd_uri_t){
            .uri = "/api/chaos/crash", .method = HTTP_POST, .handler = chaos_crash_handler });
#endif

        httpd_register_uri_handler(server, &(httpd_uri_t){
            .uri = "/api/cluster/selftest", .method = HTTP_GET, .handler = cluster_selftest_handler });

        httpd_register_uri_handler(server, &(httpd_uri_t){
            .uri = "/api/kernel/load", .method = HTTP_GET, .handler = kernel_load_handler });

        /* WIFI */
        httpd_register_uri_handler(server, &(httpd_uri_t){
            .uri = "/api/wifi", .method = HTTP_GET, .handler = wifi_save_handler });
        httpd_register_uri_handler(server, &(httpd_uri_t){
            .uri = "/api/onboarding/wifi/confirm", .method = HTTP_POST, .handler = wifi_confirm_handler });

        httpd_register_uri_handler(server, &(httpd_uri_t){
            .uri = "/api/wifi/status", .method = HTTP_GET, .handler = wifi_status_handler });

        httpd_register_uri_handler(server, &(httpd_uri_t){
            .uri = "/api/wifi/scan", .method = HTTP_GET, .handler = wifi_scan_handler });

        /* WEBSOCKET */
        httpd_register_uri_handler(server, &(httpd_uri_t){
            .uri = "/ws", .method = HTTP_GET, .handler = ws_handler, .is_websocket = true });

        /* CAPTIVE PORTAL (🔥 TODOS OS SISTEMAS) */
        httpd_register_uri_handler(server, &(httpd_uri_t){
            .uri = "/generate_204", .method = HTTP_GET, .handler = redirect_to_dash });

        httpd_register_uri_handler(server, &(httpd_uri_t){
            .uri = "/hotspot-detect.html", .method = HTTP_GET, .handler = redirect_to_dash });

        httpd_register_uri_handler(server, &(httpd_uri_t){
            .uri = "/connecttest.txt", .method = HTTP_GET, .handler = redirect_to_dash });

        httpd_register_uri_handler(server, &(httpd_uri_t){
            .uri = "/ncsi.txt", .method = HTTP_GET, .handler = redirect_to_dash });

        httpd_register_uri_handler(server, &(httpd_uri_t){
            .uri = "/fwlink", .method = HTTP_GET, .handler = redirect_to_dash });

        httpd_register_uri_handler(server, &(httpd_uri_t){
            .uri = "/bag", .method = HTTP_GET, .handler = no_content_handler });

        httpd_register_uri_handler(server, &(httpd_uri_t){
            .uri = "/favicon.ico", .method = HTTP_GET, .handler = no_content_handler });

        /* DASHBOARD - REMOVED (ENDAP STUDIO DELEGATED) */
        // dashboard_register(server);

        /* CATCH-ALL PARA CAPTIVE PORTAL NO iOS/ANDROID */
        httpd_register_err_handler(server, HTTPD_404_NOT_FOUND, redirect_to_dash_err);

        /* Inicializar mDNS para anunciar o serviço HTTP independentemente da interface ativa (Ethernet ou Wi-Fi) */
        endap_mdns_init();

        ESP_LOGI(TAG, "HTTP SERVER FINAL (CAPTIVE + DASH)");
        core_mem_audit("HTTP_POST_SERVER_START");
    }
}

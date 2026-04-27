#include "http_server.h"
#include "dashboard.h"
#include "auth.h"
#include "wifi_manager.h"
#include "cluster_manager.h"
#include "cluster_metrics.h"
#include "cluster_io.h"
#include "cluster_self_test.h"
#include "cluster_transport.h"
#include "node_registry.h"
#include "automation_engine.h"
#include "automation_node.h"
#include "bus_health_monitor.h"
#include "rs485_engine.h"
#include "rs485_master.h"
#include "kernel_metrics.h"
#include "kernel_phase_metrics.h"
#include "phase_load_test.h"
#include "phase_monitor.h"
#include "io_driver.h"
#include "input_learning.h"
#include "device_profile.h"
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

#include <inttypes.h>
#include <stdbool.h>
#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

#define TAG "HTTP"
#define HTTPD_STACK_SIZE 8192
#define HTTPD_MAX_URI_HANDLERS 80
#define AUTOMATION_JSON_BUFFER_SIZE 6144
#define STATUS_JSON_BUFFER_SIZE 24576
#define PROFILE_JSON_BUFFER_SIZE 16384
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
    vTaskDelay(pdMS_TO_TICKS(250));
    esp_restart();
}

static int ws_clients[HTTP_WS_MAX_CLIENTS];
static int ws_count = 0;
static volatile bool ws_broadcast_pending = false;
static uint64_t ws_last_periodic_us = 0;
static portMUX_TYPE ws_lock = portMUX_INITIALIZER_UNLOCKED;
static char automation_json_buffer[AUTOMATION_JSON_BUFFER_SIZE];
static char profile_json_buffer[PROFILE_JSON_BUFFER_SIZE];
static char public_profile_json_buffer[PUBLIC_PROFILE_JSON_BUFFER_SIZE];
static char nodes_json_buffer[NODES_JSON_BUFFER_SIZE];
static char network_preview_json_buffer[NETWORK_PREVIEW_JSON_BUFFER_SIZE];
static char status_json_buffer[STATUS_JSON_BUFFER_SIZE];
static char ws_status_json_buffer[STATUS_JSON_BUFFER_SIZE];
static uint32_t status_prev_deadline_miss = 0;
static uint64_t status_prev_uptime_ms = 0;
static char wifi_status_json_buffer[512];
static char wifi_scan_json_buffer[WIFI_SCAN_JSON_BUFFER_SIZE];
static automation_node_t automation_rules_snapshot[AUTOMATION_ENGINE_MAX_NODES];
static automation_rule_diag_t automation_diag_snapshot[AUTOMATION_ENGINE_MAX_NODES];
static io_binding_input_view_t input_profile_snapshot[IO_BINDING_MAX_INPUTS];
static io_binding_output_view_t output_profile_snapshot[IO_BINDING_MAX_OUTPUTS];
static io_binding_output_view_t status_output_snapshot[IO_BINDING_MAX_OUTPUTS];
static io_driver_input_diag_t status_input_diag_snapshot[STATUS_IO_MAX_CHANNELS];
static failsafe_output_status_t failsafe_status_snapshot[FAILSAFE_MAX_OUTPUTS];

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
static bool installation_map_save(void);

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

static void http_set_public_json_headers(httpd_req_t *req)
{
    http_set_private_json_headers(req);
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Methods", "GET, OPTIONS");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Headers", "Content-Type");
}

static void http_set_private_text_headers(httpd_req_t *req)
{
    http_set_common_security_headers(req);
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
    auth_status_t auth_status = {0};
    char token[AUTH_SESSION_TOKEN_LEN + 1] = {0};
    bool authenticated = false;

    if (http_auth_token_from_request(req, token, sizeof(token)))
        authenticated = auth_validate_session(token, &auth_status);
    else
        auth_get_status(NULL, &auth_status);

    if (!authenticated)
    {
        if (auth_status.bootstrap_open && !allow_password_change)
            return true;

        http_set_private_text_headers(req);
        http_auth_clear_cookie(req);
        httpd_resp_set_status(req, "401 Unauthorized");
        httpd_resp_sendstr(req, "UNAUTHORIZED");
        return false;
    }

    if (!allow_password_change && auth_status.password_change_required)
    {
        http_set_private_text_headers(req);
        httpd_resp_set_status(req, "403 Forbidden");
        httpd_resp_sendstr(req, "PASSWORD_CHANGE_REQUIRED");
        return false;
    }

    if (capability != 0U &&
        !auth_role_has_capability(auth_status.role, capability))
    {
        http_set_private_text_headers(req);
        httpd_resp_set_status(req, "403 Forbidden");
        httpd_resp_sendstr(req, "FORBIDDEN");
        return false;
    }

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
                         "\"reboot_recovery\":%s,\"runtime_diagnostics\":%s,\"security_admin\":%s}",
                         (capabilities & AUTH_CAP_DASHBOARD_READ) ? "true" : "false",
                         (capabilities & AUTH_CAP_MANUAL_IO) ? "true" : "false",
                         (capabilities & AUTH_CAP_NODE_ADMISSION) ? "true" : "false",
                         (capabilities & AUTH_CAP_PROFILE_WRITE) ? "true" : "false",
                         (capabilities & AUTH_CAP_TRANSPORT_WRITE) ? "true" : "false",
                         (capabilities & AUTH_CAP_AUTOMATION_WRITE) ? "true" : "false",
                         (capabilities & AUTH_CAP_REBOOT_RECOVERY) ? "true" : "false",
                         (capabilities & AUTH_CAP_RUNTIME_DIAGNOSTICS) ? "true" : "false",
                         (capabilities & AUTH_CAP_SECURITY_ADMIN) ? "true" : "false");
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
        nvs_commit(nvs) != ESP_OK)
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
    char *json = (char *)malloc(INSTALLATION_MAP_JSON_BUFFER_SIZE);
    size_t len = 0U;

    if (!json)
    {
        httpd_resp_set_status(req, "500 Internal Server Error");
        httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"no_memory\"}");
        return ESP_OK;
    }

    len = build_installation_map_json(json, INSTALLATION_MAP_JSON_BUFFER_SIZE);
    if (len == 0U)
    {
        free(json);
        httpd_resp_set_status(req, "500 Internal Server Error");
        httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"installation_map_build_failed\"}");
        return ESP_OK;
    }

    httpd_resp_send(req, json, len);
    free(json);
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
    snprintf(out_code, INSTALLATION_MAP_LOCAL_CODE_LEN, "%s%d", input ? "IN" : "OUT", local_index);
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
                       "\"wifi_enabled\":%u,\"ethernet_enabled\":%u,\"rs485_enabled\":%u},"
                       "\"effective_rule\":",
                       device_profile_input_count(),
                       device_profile_output_count(),
                       input_count,
                       output_count,
                       (network && network->wifi_enabled) ? 1U : 0U,
                       (network && network->ethernet_enabled) ? 1U : 0U,
                       (network && network->rs485_enabled) ? 1U : 0U))
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
    else if (strcmp(text, "diagnostics") == 0)
        *phase = PHASE_LOAD_TEST_DIAGNOSTICS;
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
    (void)failsafe_sync_outputs();
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
        io_binding_output_view_t binding = {0};
        bool has_binding = io_binding_get_output(status->output_id, &binding);
        int32_t current_value = 0;
        const char *backend_code = has_binding ? io_binding_backend_code(binding.backend) : "";

        (void)state_get_int(status->output_id, &current_value);

        if (i > 0 && !append_text(buf, buf_size, &offset, ","))
            return 0U;

        if (!append_format(buf,
                           buf_size,
                           &offset,
                           "{\"id\":%u,\"output_id\":%u,\"name\":",
                           status->output_id,
                           status->output_id))
            return 0U;

        if (!append_json_string(buf, buf_size, &offset, has_binding ? binding.name : ""))
            return 0U;

        if (!append_text(buf, buf_size, &offset, ",\"alias\":"))
            return 0U;

        if (!append_json_string(buf, buf_size, &offset, has_binding ? binding.name : ""))
            return 0U;

        if (!append_text(buf, buf_size, &offset, ",\"role\":"))
            return 0U;

        if (!append_json_string(buf, buf_size, &offset, has_binding ? binding.role : ""))
            return 0U;

        if (!append_text(buf, buf_size, &offset, ",\"backend\":"))
            return 0U;

        if (!append_json_string(buf, buf_size, &offset, backend_code ? backend_code : ""))
            return 0U;

        if (!append_text(buf, buf_size, &offset, ",\"backend_name\":"))
            return 0U;

        if (!append_json_string(buf, buf_size, &offset, has_binding ? binding.backend_name : ""))
            return 0U;

        if (!append_text(buf, buf_size, &offset, ",\"address\":"))
            return 0U;

        if (!append_json_string(buf, buf_size, &offset, has_binding ? binding.address : ""))
            return 0U;

        if (!append_format(buf,
                           buf_size,
                           &offset,
                           ",\"gpio\":%d,\"value\":%" PRId32 ",\"enabled\":%u,\"boot_action\":",
                           has_binding ? binding.gpio : -1,
                           current_value,
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

        if (!append_text(buf, buf_size, &offset, ",\"startup_mode\":"))
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

    if (!append_text(buf, buf_size, &offset, "]}"))
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
        "\"p_io\":%" PRIu32 ",\"p_fieldbus\":%" PRIu32 ",\"p_automation\":%" PRIu32 ",\"p_events\":%" PRIu32 ",\"p_diag\":%" PRIu32 ","
        "\"p_io_deadline\":%" PRIu32 ",\"p_io_apply_deadline\":%" PRIu32 ",\"p_fieldbus_deadline\":%" PRIu32 ","
        "\"p_automation_deadline\":%" PRIu32 ",\"p_events_deadline\":%" PRIu32 ",\"p_diag_deadline\":%" PRIu32 ","
        "\"p_io_overruns\":%" PRIu32 ",\"p_io_apply_overruns\":%" PRIu32 ",\"p_fieldbus_overruns\":%" PRIu32 ","
        "\"p_automation_overruns\":%" PRIu32 ",\"p_events_overruns\":%" PRIu32 ",\"p_diag_overruns\":%" PRIu32 ","
        "\"lt_active\":%u,\"lt_phase_count\":%" PRIu32 ",\"lt_total_us\":%" PRIu32 ","
        "\"lt_io_us\":%" PRIu32 ",\"lt_io_apply_us\":%" PRIu32 ",\"lt_fieldbus_us\":%" PRIu32 ","
        "\"lt_automation_us\":%" PRIu32 ",\"lt_events_us\":%" PRIu32 ",\"lt_diag_us\":%" PRIu32 ","
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
        phase.io_max, phase_fieldbus_max, phase.automation_max, phase.events_max, phase.diagnostics_max,
        phase_mon.io_deadline_us, phase_mon.io_apply_deadline_us, phase_fieldbus_deadline,
        phase_mon.automation_deadline_us, phase_mon.events_deadline_us, phase_mon.diagnostics_deadline_us,
        phase_mon.io_overruns, phase_mon.io_apply_overruns, phase_fieldbus_overruns,
        phase_mon.automation_overruns, phase_mon.events_overruns, phase_mon.diagnostics_overruns,
        load_test.active ? 1U : 0U, (uint32_t)load_test.active_phase_count, load_test.total_us,
        load_test.io_us, load_test.io_apply_us, load_test_fieldbus_us,
        load_test.automation_us, load_test.events_us, load_test.diagnostics_us,
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

        if (!append_text(buf, buf_size, &offset, ",\"description\":"))
            return 0;

        if (!append_json_string(buf, buf_size, &offset, binding->description))
            return 0;

        if (!append_text(buf, buf_size, &offset, "}"))
            return 0;
    }

    if (!append_text(buf, buf_size, &offset, "],\"outputs\":["))
        return 0;

    for (int i = 0; i < output_count; i++)
    {
        const io_binding_output_view_t *output = &status_output_snapshot[i];
        const device_output_profile_t *profile = device_profile_find_output(output->id);
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

        if (!append_text(buf, buf_size, &offset, "}"))
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
        if (!append_text(buf, buf_size, &offset, ",\"description\":"))
            return offset;
        if (!append_json_string(buf, buf_size, &offset, binding->description))
            return offset;
        if (!append_text(buf, buf_size, &offset, "}"))
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
        if (!append_text(buf, buf_size, &offset, "}"))
            return offset;

        first = false;
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
                       "\"wifi_supported\":%u,\"wifi_enabled\":%u,\"ethernet_supported\":%u,\"ethernet_enabled\":%u,\"rs485_supported\":%u,\"rs485_enabled\":%u,\"ethernet_configured\":%u,\"ethernet_mode\":",
                       (network && network->wifi_supported) ? 1U : 0U,
                       (network && network->wifi_enabled) ? 1U : 0U,
                       (network && network->ethernet_supported) ? 1U : 0U,
                       (network && network->ethernet_enabled) ? 1U : 0U,
                       (network && network->rs485_supported) ? 1U : 0U,
                       (network && network->rs485_enabled) ? 1U : 0U,
                       ethernet_configured ? 1U : 0U))
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
        if (!append_text(buf, buf_size, &offset, ",\"description\":"))
            return 0;
        if (!append_json_string(buf, buf_size, &offset, binding->description))
            return 0;
        if (!append_text(buf, buf_size, &offset, "}"))
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
        if (!append_text(buf, buf_size, &offset, "}"))
            return 0;

        first = false;
    }

    if (!append_text(buf, buf_size, &offset, "]"))
        return 0;

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

    if (!append_format(buf,
                       buf_size,
                       &offset,
                       ",\"input_slot_capacity\":%d,\"output_slot_capacity\":%d,\"active_input_count\":%d,\"active_output_count\":%d,\"network\":{\"wifi_supported\":%u,\"wifi_enabled\":%u,\"ethernet_supported\":%u,\"ethernet_enabled\":%u,\"rs485_supported\":%u,\"rs485_enabled\":%u}",
                       device_profile_input_count(),
                       device_profile_output_count(),
                       input_count,
                       output_count,
                       (network && network->wifi_supported) ? 1U : 0U,
                       (network && network->wifi_enabled) ? 1U : 0U,
                       (network && network->ethernet_supported) ? 1U : 0U,
                       (network && network->ethernet_enabled) ? 1U : 0U,
                       (network && network->rs485_supported) ? 1U : 0U,
                       (network && network->rs485_enabled) ? 1U : 0U))
        return 0;

    if (!append_text(buf, buf_size, &offset, "}"))
        return 0;

    return offset;
}

static esp_err_t public_profile_handler(httpd_req_t *req)
{
    size_t len = build_public_profile_json(public_profile_json_buffer, sizeof(public_profile_json_buffer));

    http_set_public_json_headers(req);

    if (len == 0U)
        httpd_resp_send(req, "{\"inputs\":[],\"outputs\":[]}", HTTPD_RESP_USE_STRLEN);
    else
        httpd_resp_send(req, public_profile_json_buffer, len);

    return ESP_OK;
}

/* ============================================================
   WIFI SAVE
============================================================ */

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
    size_t len = build_wifi_scan_json(wifi_scan_json_buffer, sizeof(wifi_scan_json_buffer));

    if (!http_auth_require_cap(req, AUTH_CAP_TRANSPORT_WRITE))
        return ESP_OK;

    http_set_private_json_headers(req);

    if (len == 0U)
        httpd_resp_send(req,
                        "{\"ok\":false,\"count\":0,\"error\":\"BUFFER\",\"networks\":[]}",
                        HTTPD_RESP_USE_STRLEN);
    else
        httpd_resp_send(req, wifi_scan_json_buffer, len);

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

    size_t msg_len = build_status_json(ws_status_json_buffer, sizeof(ws_status_json_buffer));

    httpd_ws_frame_t frame = {
        .type = HTTPD_WS_TYPE_TEXT,
        .payload = (uint8_t*)ws_status_json_buffer,
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

    if (!http_auth_require_cap(req, AUTH_CAP_MANUAL_IO))
        return ESP_OK;

    http_set_private_text_headers(req);

    if (!query_get_int(req, "id", 0, UINT16_MAX, &id) ||
        !query_get_int(req, "value", 0, 1, &value) ||
        !query_get_optional_u32(req, "target_node", &target_node, &target_found))
    {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_sendstr(req, "BAD_REQUEST");
        return ESP_OK;
    }

    if (!device_profile_is_valid_output((uint16_t)id))
    {
        httpd_resp_set_status(req, "404 Not Found");
        httpd_resp_sendstr(req, "NOT_FOUND");
        return ESP_OK;
    }

    metrics = cluster_get_metrics();
    explicit_remote_target = target_found &&
                             target_node != 0U &&
                             metrics.self_node != 0U &&
                             target_node != metrics.self_node;

    if (explicit_remote_target ||
        !cluster_io_is_local((uint16_t)id))
    {
        protocol_msg_t msg = {0};
        uint32_t owner = 0U;

        if (metrics.self_node == 0U)
        {
            httpd_resp_set_status(req, "409 Conflict");
            httpd_resp_sendstr(req, "SELF_NODE_UNAVAILABLE");
            return ESP_OK;
        }

        owner = (target_found && target_node != 0U) ? target_node : cluster_io_get_owner((uint16_t)id);

        if (owner == 0U || owner == metrics.self_node)
        {
            httpd_resp_set_status(req, "409 Conflict");
            httpd_resp_sendstr(req, "REMOTE_OWNER_UNRESOLVED");
            return ESP_OK;
        }

        if (!node_registry_is_operational(owner))
        {
            httpd_resp_set_status(req, "409 Conflict");
            httpd_resp_sendstr(req, "REMOTE_OWNER_OFFLINE");
            return ESP_OK;
        }

        if (!cluster_transport_is_ready())
        {
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
        httpd_resp_set_status(req, "409 Conflict");
        httpd_resp_sendstr(req, "FAILSAFE_ACTIVE");
        return ESP_OK;
    }

    if (!io_command_push(id, effective_value))
    {
        httpd_resp_set_status(req, "503 Service Unavailable");
        httpd_resp_sendstr(req, "BUSY");
        return ESP_OK;
    }

    snprintf(audit_detail, sizeof(audit_detail), "output=%d value=%" PRId32, id, effective_value);
    auth_audit_log("manual_output_command", audit_detail);
    http_server_notify_state_change();
    httpd_resp_set_status(req, "202 Accepted");
    httpd_resp_sendstr(req, "QUEUED");

    return ESP_OK;
}

static esp_err_t status_handler(httpd_req_t *req)
{
    size_t len;

    if (!http_auth_require_cap(req, AUTH_CAP_DASHBOARD_READ))
        return ESP_OK;

    http_set_private_json_headers(req);
    len = build_status_json(status_json_buffer, sizeof(status_json_buffer));

    if (len == 0U)
        httpd_resp_send(req, "{}", 2);
    else
        httpd_resp_send(req, status_json_buffer, len);

    return ESP_OK;
}

static esp_err_t public_status_handler(httpd_req_t *req)
{
    size_t len;

    http_set_public_json_headers(req);
    len = build_public_status_json(status_json_buffer, sizeof(status_json_buffer));

    if (len == 0U)
        httpd_resp_send(req, "{}", 2);
    else
        httpd_resp_send(req, status_json_buffer, len);

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

static esp_err_t failsafe_handler(httpd_req_t *req)
{
    char *json;
    size_t len;

    if (!http_auth_require_cap(req, AUTH_CAP_DASHBOARD_READ))
        return ESP_OK;

    http_set_private_json_headers(req);
    json = (char *)malloc(INSTALLATION_MAP_JSON_BUFFER_SIZE);
    if (!json)
        return failsafe_json_error(req, "500 Internal Server Error", "no_memory");

    len = build_failsafe_json(json, INSTALLATION_MAP_JSON_BUFFER_SIZE);

    if (len == 0U)
        httpd_resp_send(req, "{\"ok\":true,\"count\":0,\"outputs\":[]}", HTTPD_RESP_USE_STRLEN);
    else
        httpd_resp_send(req, json, len);

    free(json);
    return ESP_OK;
}

static esp_err_t failsafe_save_handler(httpd_req_t *req)
{
    char body[HTTP_BODY_BUFFER_SIZE] = {0};
    char output_text[16] = {0};
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
    int output_id = 0;
    int enabled = 1;
    int safe_value = 0;
    int manual_value = 0;

    if (!http_auth_require_cap(req, AUTH_CAP_PROFILE_WRITE))
        return ESP_OK;

    http_set_private_json_headers(req);

    if (!http_read_request_body(req, body, sizeof(body)) ||
        !http_body_get_value(body, "output_id", output_text, sizeof(output_text)) ||
        !query_parse_int(output_text, 0, UINT16_MAX, &output_id))
    {
        return failsafe_json_error(req, "400 Bad Request", "bad_request");
    }

    if (!failsafe_get_output_policy((uint16_t)output_id, &current))
    {
        return failsafe_json_error(req, "404 Not Found", "invalid_output");
    }

    enabled = current.enabled ? 1 : 0;
    boot_action = current.boot_action;
    comm_loss_action = current.comm_loss_action;
    runtime_fault_action = current.runtime_fault_action;
    safe_value = current.safe_value ? 1 : 0;
    recovery_mode = current.recovery_mode;

    if (http_body_get_value(body, "enabled", enabled_text, sizeof(enabled_text)) &&
        !query_parse_int(enabled_text, 0, 1, &enabled))
    {
        return failsafe_json_error(req, "400 Bad Request", "invalid_enabled");
    }

    if (!http_body_get_value(body, "boot_action", boot_text, sizeof(boot_text)))
        (void)http_body_get_value(body, "startup_mode", boot_text, sizeof(boot_text));

    if (boot_text[0] != '\0' &&
        !failsafe_action_from_code(boot_text, &boot_action))
    {
        return failsafe_json_error(req, "400 Bad Request", "invalid_boot_action");
    }

    if (!http_body_get_value(body, "comm_loss_action", comm_loss_text, sizeof(comm_loss_text)))
        (void)http_body_get_value(body, "comm_loss_mode", comm_loss_text, sizeof(comm_loss_text));

    if (comm_loss_text[0] != '\0' &&
        !failsafe_action_from_code(comm_loss_text, &comm_loss_action))
    {
        return failsafe_json_error(req, "400 Bad Request", "invalid_comm_loss_action");
    }

    if (http_body_get_value(body, "runtime_fault_action", runtime_fault_text, sizeof(runtime_fault_text)) &&
        !failsafe_action_from_code(runtime_fault_text, &runtime_fault_action))
    {
        return failsafe_json_error(req, "400 Bad Request", "invalid_runtime_fault_action");
    }

    if (http_body_get_value(body, "safe_value", safe_value_text, sizeof(safe_value_text)) &&
        !query_parse_int(safe_value_text, 0, 1, &safe_value))
    {
        return failsafe_json_error(req, "400 Bad Request", "invalid_safe_value");
    }

    if (http_body_get_value(body, "recovery_mode", recovery_text, sizeof(recovery_text)) &&
        !failsafe_recovery_from_code(recovery_text, &recovery_mode))
    {
        return failsafe_json_error(req, "400 Bad Request", "invalid_recovery_mode");
    }
    else if (!recovery_text[0] &&
             (http_body_get_value(body, "manual_rearm", manual_text, sizeof(manual_text)) ||
              http_body_get_value(body, "manual_reset_required", manual_text, sizeof(manual_text))))
    {
        if (!query_parse_int(manual_text, 0, 1, &manual_value))
            return failsafe_json_error(req, "400 Bad Request", "invalid_manual_reset");

        recovery_mode = manual_value ? FAILSAFE_RECOVERY_MANUAL : FAILSAFE_RECOVERY_AUTO;
    }

    if (!failsafe_set_output_policy((uint16_t)output_id,
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
             "output=%d enabled=%d boot=%s comm_loss=%s runtime_fault=%s safe=%d recovery=%s",
             output_id,
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
    char output_text[16] = {0};
    char audit_detail[64];
    int output_id = 0;

    if (!http_auth_require_cap(req, AUTH_CAP_PROFILE_WRITE))
        return ESP_OK;

    http_set_private_json_headers(req);

    if (req->content_len > 0)
    {
        if (!http_read_request_body(req, body, sizeof(body)) ||
            !http_body_get_value(body, "output_id", output_text, sizeof(output_text)) ||
            !query_parse_int(output_text, 0, UINT16_MAX, &output_id))
        {
            return failsafe_json_error(req, "400 Bad Request", "bad_request");
        }
    }
    else if (!query_get_int(req, "output_id", 0, UINT16_MAX, &output_id))
    {
        return failsafe_json_error(req, "400 Bad Request", "bad_request");
    }

    if (!failsafe_rearm((uint16_t)output_id))
        return failsafe_json_error(req, "404 Not Found", "invalid_output");

    snprintf(audit_detail, sizeof(audit_detail), "output=%d", output_id);
    auth_audit_log("failsafe_rearmed", audit_detail);
    http_server_notify_state_change();
    httpd_resp_sendstr(req, "{\"ok\":true}");
    return ESP_OK;
}

static esp_err_t failsafe_test_handler(httpd_req_t *req)
{
    char body[HTTP_BODY_BUFFER_SIZE] = {0};
    char output_text[16] = {0};
    char audit_detail[96];
    int output_id = 0;
    int32_t safe_value = 0;

    if (!http_auth_require_cap(req, AUTH_CAP_PROFILE_WRITE))
        return ESP_OK;

    http_set_private_json_headers(req);

    if (!http_read_request_body(req, body, sizeof(body)) ||
        !http_body_get_value(body, "output_id", output_text, sizeof(output_text)) ||
        !query_parse_int(output_text, 0, UINT16_MAX, &output_id))
    {
        return failsafe_json_error(req, "400 Bad Request", "bad_request");
    }

    if (!failsafe_trigger_manual_test((uint16_t)output_id, &safe_value))
        return failsafe_json_error(req, "404 Not Found", "invalid_output");

    (void)state_set_int((uint16_t)output_id, safe_value);

    snprintf(audit_detail,
             sizeof(audit_detail),
             "output=%d reason=%s value=%" PRId32,
             output_id,
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

    http_set_private_json_headers(req);

    if (len == 0U)
        httpd_resp_send(req, "{}", 2);
    else
        httpd_resp_send(req, wifi_status_json_buffer, len);

    return ESP_OK;
}

static esp_err_t profile_handler(httpd_req_t *req)
{
    size_t len = build_profile_json(profile_json_buffer, sizeof(profile_json_buffer));

    if (!http_auth_require_cap(req, AUTH_CAP_DASHBOARD_READ))
        return ESP_OK;

    http_set_private_json_headers(req);

    if (len == 0U)
        httpd_resp_send(req, "{}", 2);
    else
        httpd_resp_send(req, profile_json_buffer, len);

    return ESP_OK;
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

static esp_err_t network_config_handler(httpd_req_t *req)
{
    const device_network_profile_t *network = device_profile_network();
    device_profile_network_config_result_t result;
    int wifi_enabled = 0;
    int ethernet_enabled = 0;
    int rs485_enabled = 0;
    bool wifi_found = false;
    bool ethernet_found = false;
    bool rs485_found = false;
    char audit_detail[96];

    if (!http_auth_require_cap(req, AUTH_CAP_TRANSPORT_WRITE))
        return ESP_OK;

    http_set_private_text_headers(req);

    if (!network ||
        !query_get_optional_int(req, "wifi_enabled", 0, 1, &wifi_enabled, &wifi_found) ||
        !query_get_optional_int(req, "ethernet_enabled", 0, 1, &ethernet_enabled, &ethernet_found) ||
        !query_get_optional_int(req, "rs485_enabled", 0, 1, &rs485_enabled, &rs485_found))
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

    result = device_profile_set_network_enabled(wifi_enabled != 0,
                                                ethernet_enabled != 0,
                                                rs485_enabled != 0);
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

    len = build_network_preview_json(network_preview_json_buffer,
                                     sizeof(network_preview_json_buffer),
                                     wifi_enabled != 0,
                                     ethernet_enabled != 0,
                                     rs485_enabled != 0);

    if (len == 0U)
        httpd_resp_send(req, "{\"error\":\"BUFFER\"}", HTTPD_RESP_USE_STRLEN);
    else
        httpd_resp_send(req, network_preview_json_buffer, len);

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

static esp_err_t nodes_handler(httpd_req_t *req)
{
    size_t len = build_nodes_json(nodes_json_buffer, sizeof(nodes_json_buffer));

    if (!http_auth_require_cap(req, AUTH_CAP_DASHBOARD_READ))
        return ESP_OK;

    http_set_private_json_headers(req);

    if (len == 0U)
        httpd_resp_send(req, "{\"nodes\":[]}", HTTPD_RESP_USE_STRLEN);
    else
        httpd_resp_send(req, nodes_json_buffer, len);

    return ESP_OK;
}

static bool query_get_channel_backend(httpd_req_t *req, device_channel_backend_t *out_backend)
{
    char backend_text[24] = {0};
    bool found = false;

    if (!out_backend)
        return false;

    *out_backend = DEVICE_CHANNEL_BACKEND_GPIO;

    if (!query_get_optional_str(req, "backend", backend_text, sizeof(backend_text), &found))
        return false;

    if (!found || backend_text[0] == '\0' || strcmp(backend_text, "gpio") == 0)
    {
        *out_backend = DEVICE_CHANNEL_BACKEND_GPIO;
        return true;
    }

    if (strcmp(backend_text, "mcp23x17") == 0 || strcmp(backend_text, "mcp") == 0)
    {
        *out_backend = DEVICE_CHANNEL_BACKEND_MCP23X17;
        return true;
    }

    return false;
}

static bool query_get_binding_address(httpd_req_t *req,
                                      device_channel_backend_t backend,
                                      int *gpio,
                                      int *backend_instance,
                                      int *endpoint_index)
{
    if (!gpio || !backend_instance || !endpoint_index)
        return false;

    *gpio = -1;
    *backend_instance = 0;
    *endpoint_index = 0;

    if (backend == DEVICE_CHANNEL_BACKEND_GPIO)
    {
        return query_get_optional_int(req, "gpio", -1, INT16_MAX, gpio, NULL);
    }

    if (backend == DEVICE_CHANNEL_BACKEND_MCP23X17)
    {
        return query_get_optional_int(req, "backend_instance", 0, INT16_MAX, backend_instance, NULL) &&
               query_get_optional_int(req, "endpoint_index", 0, INT16_MAX, endpoint_index, NULL);
    }

    return false;
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

        case IO_BINDING_RESULT_INVALID_BACKEND:
            httpd_resp_set_status(req, "409 Conflict");
            httpd_resp_sendstr(req, "INVALID_BACKEND");
            return ESP_FAIL;

        case IO_BINDING_RESULT_UNSUPPORTED_BACKEND:
            httpd_resp_set_status(req, "409 Conflict");
            httpd_resp_sendstr(req, "UNSUPPORTED_BACKEND");
            return ESP_FAIL;

        case IO_BINDING_RESULT_INVALID_ENDPOINT:
            httpd_resp_set_status(req, "409 Conflict");
            httpd_resp_sendstr(req, "INVALID_ENDPOINT");
            return ESP_FAIL;

        case IO_BINDING_RESULT_ADDRESS_CONFLICT:
            httpd_resp_set_status(req, "409 Conflict");
            httpd_resp_sendstr(req, "ADDRESS_CONFLICT");
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
    int backend_instance = 0;
    int endpoint_index = 0;
    device_channel_backend_t backend = DEVICE_CHANNEL_BACKEND_GPIO;
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
        !query_get_channel_backend(req, &backend) ||
        !query_get_binding_address(req, backend, &gpio, &backend_instance, &endpoint_index))
    {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_sendstr(req, "BAD_REQUEST");
        return ESP_OK;
    }

    result = io_binding_set_output_ex((uint16_t)id, name, role, backend, gpio, backend_instance, endpoint_index);

    if (respond_binding_result(req, result) != ESP_OK)
        return ESP_OK;

    snprintf(audit_detail, sizeof(audit_detail), "output=%d backend=%s address=%d:%d gpio=%d name=%s", id, io_binding_backend_code(backend), backend_instance, endpoint_index, gpio, name[0] ? name : "-");
    auth_audit_log("output_config_updated", audit_detail);
    http_server_notify_state_change();
    httpd_resp_sendstr(req, "SAVED");
    return ESP_OK;
}

static esp_err_t input_config_handler(httpd_req_t *req)
{
    int id = 0;
    int gpio = -1;
    int backend_instance = 0;
    int endpoint_index = 0;
    device_channel_backend_t backend = DEVICE_CHANNEL_BACKEND_GPIO;
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
        !query_get_channel_backend(req, &backend) ||
        !query_get_binding_address(req, backend, &gpio, &backend_instance, &endpoint_index))
    {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_sendstr(req, "BAD_REQUEST");
        return ESP_OK;
    }

    result = io_binding_set_input_ex((uint16_t)id, name, role, backend, gpio, backend_instance, endpoint_index);

    if (respond_binding_result(req, result) != ESP_OK)
        return ESP_OK;

    snprintf(audit_detail, sizeof(audit_detail), "input=%d backend=%s address=%d:%d gpio=%d name=%s", id, io_binding_backend_code(backend), backend_instance, endpoint_index, gpio, name[0] ? name : "-");
    auth_audit_log("input_config_updated", audit_detail);
    http_server_notify_state_change();
    httpd_resp_sendstr(req, "SAVED");
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
    int backend_instance = 0;
    int endpoint_index = 0;
    device_channel_backend_t backend = DEVICE_CHANNEL_BACKEND_GPIO;
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
        !query_get_channel_backend(req, &backend) ||
        !query_get_binding_address(req, backend, &gpio, &backend_instance, &endpoint_index))
    {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_sendstr(req, "BAD_REQUEST");
        return ESP_OK;
    }

    result = io_binding_add_input_ex((uint16_t)id, name, role, backend, gpio, backend_instance, endpoint_index);

    if (respond_binding_result(req, result) != ESP_OK)
        return ESP_OK;

    snprintf(audit_detail, sizeof(audit_detail), "input=%d backend=%s address=%d:%d gpio=%d", id, io_binding_backend_code(backend), backend_instance, endpoint_index, gpio);
    auth_audit_log("input_added", audit_detail);
    http_server_notify_state_change();
    httpd_resp_sendstr(req, "ADDED");
    return ESP_OK;
}

static esp_err_t output_add_handler(httpd_req_t *req)
{
    int id = 0;
    int gpio = -1;
    int backend_instance = 0;
    int endpoint_index = 0;
    device_channel_backend_t backend = DEVICE_CHANNEL_BACKEND_GPIO;
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
        !query_get_channel_backend(req, &backend) ||
        !query_get_binding_address(req, backend, &gpio, &backend_instance, &endpoint_index))
    {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_sendstr(req, "BAD_REQUEST");
        return ESP_OK;
    }

    result = io_binding_add_output_ex((uint16_t)id, name, role, backend, gpio, backend_instance, endpoint_index);

    if (respond_binding_result(req, result) != ESP_OK)
        return ESP_OK;

    (void)failsafe_sync_outputs();
    snprintf(audit_detail, sizeof(audit_detail), "output=%d backend=%s address=%d:%d gpio=%d", id, io_binding_backend_code(backend), backend_instance, endpoint_index, gpio);
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

    (void)failsafe_sync_outputs();
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

static esp_err_t node_adopt_handler(httpd_req_t *req)
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

static esp_err_t node_configure_handler(httpd_req_t *req)
{
    uint32_t id = 0U;
    char profile[NODE_REGISTRY_PROFILE_LEN] = {0};
    char template_name[NODE_REGISTRY_TEMPLATE_LEN] = {0};
    char audit_detail[96];

    if (!http_auth_require_cap(req, AUTH_CAP_NODE_ADMISSION))
        return ESP_OK;

    http_set_private_text_headers(req);

    if (!query_get_u32(req, "id", &id) ||
        !query_get_str(req, "profile", profile, sizeof(profile)) ||
        !query_get_optional_str(req, "template", template_name, sizeof(template_name), NULL))
    {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_sendstr(req, "BAD_REQUEST");
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

    if (!query_get_optional_str(req, "action", action, sizeof(action), NULL))
    {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_sendstr(req, "BAD_REQUEST");
        return ESP_OK;
    }

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
    size_t len = build_automation_json(automation_json_buffer, sizeof(automation_json_buffer));

    if (!http_auth_require_cap(req, AUTH_CAP_DASHBOARD_READ))
        return ESP_OK;

    http_set_private_json_headers(req);

    if (len == 0U)
        httpd_resp_send(req, "{\"saved\":0,\"count\":0,\"rules\":[]}", HTTPD_RESP_USE_STRLEN);
    else
        httpd_resp_send(req, automation_json_buffer, len);

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

    if (!query_get_int(req, "input", 0, UINT16_MAX, &input) ||
        !query_get_int(req, "threshold", INT_MIN, INT_MAX, &threshold) ||
        !query_get_int(req, "output", 0, UINT16_MAX, &output))
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

    snprintf(audit_detail,
             sizeof(audit_detail),
             "input=%d output=%d mode=%s",
             input,
             output,
             mode_text[0] ? mode_text : automation_engine_mode_to_code(mode));
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
    auth_audit_log("automation_cleared", "all-rules-cleared");
    http_server_notify_state_change();
    httpd_resp_sendstr(req, "CLEARED");
    return ESP_OK;
}

/* ============================================================
   START SERVER (🔥 FINAL)
============================================================ */

void http_server_start(void)
{
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
            .uri = "/api/set", .method = HTTP_GET, .handler = set_handler });

        httpd_register_uri_handler(server, &(httpd_uri_t){
            .uri = "/api/status", .method = HTTP_GET, .handler = status_handler });

        httpd_register_uri_handler(server, &(httpd_uri_t){
            .uri = "/api/public/status", .method = HTTP_GET, .handler = public_status_handler });

        httpd_register_uri_handler(server, &(httpd_uri_t){
            .uri = "/api/failsafe", .method = HTTP_GET, .handler = failsafe_handler });

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
            .uri = "/api/network/config", .method = HTTP_GET, .handler = network_config_handler });

        httpd_register_uri_handler(server, &(httpd_uri_t){
            .uri = "/api/network/preview", .method = HTTP_GET, .handler = network_preview_handler });

        httpd_register_uri_handler(server, &(httpd_uri_t){
            .uri = "/api/reboot", .method = HTTP_GET, .handler = reboot_handler });

        httpd_register_uri_handler(server, &(httpd_uri_t){
            .uri = "/api/recovery", .method = HTTP_GET, .handler = recovery_handler });

        httpd_register_uri_handler(server, &(httpd_uri_t){
            .uri = "/api/nodes", .method = HTTP_GET, .handler = nodes_handler });

        httpd_register_uri_handler(server, &(httpd_uri_t){
            .uri = "/api/nodes/adopt", .method = HTTP_GET, .handler = node_adopt_handler });

        httpd_register_uri_handler(server, &(httpd_uri_t){
            .uri = "/api/nodes/configure", .method = HTTP_GET, .handler = node_configure_handler });

        httpd_register_uri_handler(server, &(httpd_uri_t){
            .uri = "/api/nodes/activate", .method = HTTP_GET, .handler = node_activate_handler });

        httpd_register_uri_handler(server, &(httpd_uri_t){
            .uri = "/api/nodes/revoke", .method = HTTP_GET, .handler = node_revoke_handler });

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
            .uri = "/api/automation/remove", .method = HTTP_GET, .handler = automation_remove_handler });

        httpd_register_uri_handler(server, &(httpd_uri_t){
            .uri = "/api/automation/clear", .method = HTTP_GET, .handler = automation_clear_handler });

        httpd_register_uri_handler(server, &(httpd_uri_t){
            .uri = "/api/cluster/selftest", .method = HTTP_GET, .handler = cluster_selftest_handler });

        httpd_register_uri_handler(server, &(httpd_uri_t){
            .uri = "/api/kernel/load", .method = HTTP_GET, .handler = kernel_load_handler });

        /* WIFI */
        httpd_register_uri_handler(server, &(httpd_uri_t){
            .uri = "/api/wifi", .method = HTTP_GET, .handler = wifi_save_handler });

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

        /* DASHBOARD */
        dashboard_register(server);

        ESP_LOGI(TAG, "HTTP SERVER FINAL (CAPTIVE + DASH)");
    }
}

#include "dashboard.h"

#include "esp_http_server.h"
#include "esp_log.h"

#include <stddef.h>
#include <stdint.h>

#define TAG "DASH"

extern const uint8_t _binary_index_html_start[] asm("_binary_index_html_start");
extern const uint8_t _binary_index_html_end[]   asm("_binary_index_html_end");

static esp_err_t dashboard_handler(httpd_req_t *req)
{
    const char *html = (const char *)_binary_index_html_start;
    const size_t html_len = (size_t)(_binary_index_html_end - _binary_index_html_start);

    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store, no-cache, must-revalidate, max-age=0");
    httpd_resp_set_hdr(req, "Pragma", "no-cache");
    httpd_resp_set_hdr(req, "X-Content-Type-Options", "nosniff");

    return httpd_resp_send(req, html, (ssize_t)html_len);
}

void dashboard_register(httpd_handle_t server)
{
    if (server == NULL)
        return;

    httpd_uri_t dash = {
        .uri = "/dash",
        .method = HTTP_GET,
        .handler = dashboard_handler,
        .user_ctx = NULL,
    };

    esp_err_t err = httpd_register_uri_handler(server, &dash);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "Falha ao registrar dashboard: %s", esp_err_to_name(err));
        return;
    }

    ESP_LOGI(TAG, "Dashboard registrada em /dash");
}

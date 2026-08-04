#include "dashboard.h"

#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_timer.h"

#include <errno.h>
#include <string.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/socket.h>
#include <arpa/inet.h>

#define TAG "DASH"

extern const uint8_t _binary_index_html_gz_start[] asm("_binary_index_html_gz_start");
extern const uint8_t _binary_index_html_gz_end[]   asm("_binary_index_html_gz_end");

static void get_client_ip(httpd_req_t *req, char *ip_buf, size_t ip_buf_size)
{
    strncpy(ip_buf, "desconhecido", ip_buf_size - 1);
    ip_buf[ip_buf_size - 1] = '\0';

    int sockfd = httpd_req_to_sockfd(req);
    if (sockfd >= 0) {
        struct sockaddr_in addr;
        socklen_t addr_len = sizeof(addr);
        if (getpeername(sockfd, (struct sockaddr *)&addr, &addr_len) == 0) {
            inet_ntoa_r(addr.sin_addr, ip_buf, ip_buf_size);
        }
    }
}

static esp_err_t dashboard_handler(httpd_req_t *req)
{
    if (req == NULL) {
        ESP_LOGE(TAG, "[DASH ERRO CRITICO] httpd_req_t *req eh NULL");
        return ESP_FAIL;
    }

    int64_t start_time_us = esp_timer_get_time();
    char client_ip[32] = {0};
    get_client_ip(req, client_ip, sizeof(client_ip));
    int sockfd = httpd_req_to_sockfd(req);

    // Extrair Headers HTTP do Cliente
    char user_agent[128] = "N/A";
    char connection_hdr[32] = "N/A";
    char accept_encoding[64] = "N/A";

    if (httpd_req_get_hdr_value_str(req, "User-Agent", user_agent, sizeof(user_agent)) != ESP_OK) {
        strncpy(user_agent, "ausente", sizeof(user_agent) - 1);
    }
    if (httpd_req_get_hdr_value_str(req, "Connection", connection_hdr, sizeof(connection_hdr)) != ESP_OK) {
        strncpy(connection_hdr, "ausente", sizeof(connection_hdr) - 1);
    }
    if (httpd_req_get_hdr_value_str(req, "Accept-Encoding", accept_encoding, sizeof(accept_encoding)) != ESP_OK) {
        strncpy(accept_encoding, "ausente", sizeof(accept_encoding) - 1);
    }

    const char *html_gz = (const char *)_binary_index_html_gz_start;
    const size_t html_gz_len = (size_t)(_binary_index_html_gz_end - _binary_index_html_gz_start);

    if (html_gz == NULL || html_gz_len == 0) {
        ESP_LOGE(TAG, "[DASH ERRO CRITICO] Ponteiro binario ou tamanho invalido (_start=%p, len=%zu)",
                 html_gz, html_gz_len);
        return ESP_FAIL;
    }

    esp_err_t err;

    err = httpd_resp_set_type(req, "text/html; charset=utf-8");
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "[DASH ERRO] httpd_resp_set_type falhou (%s) no URI %s", esp_err_to_name(err), req->uri);
        return err;
    }

    err = httpd_resp_set_hdr(req, "Content-Encoding", "gzip");
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "[DASH ERRO] httpd_resp_set_hdr(Content-Encoding) falhou (%s) no URI %s", esp_err_to_name(err), req->uri);
        return err;
    }

    err = httpd_resp_set_hdr(req, "Cache-Control", "no-store, no-cache, must-revalidate, max-age=0");
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "[DASH ERRO] httpd_resp_set_hdr(Cache-Control) falhou (%s) no URI %s", esp_err_to_name(err), req->uri);
        return err;
    }

    err = httpd_resp_set_hdr(req, "Pragma", "no-cache");
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "[DASH ERRO] httpd_resp_set_hdr(Pragma) falhou (%s) no URI %s", esp_err_to_name(err), req->uri);
        return err;
    }

    err = httpd_resp_set_hdr(req, "X-Content-Type-Options", "nosniff");
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "[DASH ERRO] httpd_resp_set_hdr(X-Content-Type-Options) falhou (%s) no URI %s", esp_err_to_name(err), req->uri);
        return err;
    }

    ESP_LOGI(TAG, "[DASH REQ] IP: %s | Sock: %d | URI: %s | Conn: %s | Enc: %s | UA: %.40s...",
             client_ip, sockfd, req->uri, connection_hdr, accept_encoding, user_agent);

    // Limpa errno imediatamente antes da operacao de socket
    errno = 0;
    err = httpd_resp_send(req, html_gz, (ssize_t)html_gz_len);
    int saved_errno = errno;
    uint32_t elapsed_ms = (uint32_t)((esp_timer_get_time() - start_time_us) / 1000);
    double throughput_kbps = (elapsed_ms > 0) ? (((double)html_gz_len / 1024.0) / ((double)elapsed_ms / 1000.0)) : 0.0;

    if (err != ESP_OK) {
        const char *classificacao_neutra = "DESCONHECIDO";
        if (saved_errno == ECONNRESET || saved_errno == EPIPE || saved_errno == ENOTCONN) {
            classificacao_neutra = "DESCONEXAO_CLIENTE_DETECTADA_PELO_SOQUETE";
        } else if (saved_errno == EAGAIN || saved_errno == EWOULDBLOCK || saved_errno == ETIMEDOUT) {
            classificacao_neutra = "TIMEOUT_DE_SOQUETE_LWIP";
        } else if (saved_errno == ENOMEM || saved_errno == ENOBUFS) {
            classificacao_neutra = "EXAUSTAO_DE_MEMORIA_LWIP";
        } else if (saved_errno != 0) {
            classificacao_neutra = "ERRO_SOQUETE_LWIP_OUTROS";
        } else {
            classificacao_neutra = "ERRO_INTERNO_HTTPD_SERVER";
        }

        ESP_LOGE(TAG, "[DIAGNOSTICO RCA PRECISO]");
        ESP_LOGE(TAG, "  URI: %s | IP: %s | Sock: %d", req->uri, client_ip, sockfd);
        ESP_LOGE(TAG, "  Tamanho Total: %zu B | Tempo Envio: %lu ms | Throughput: %.2f KB/s",
                 html_gz_len, (unsigned long)elapsed_ms, throughput_kbps);
        ESP_LOGE(TAG, "  Headers -> Conn: %s | Enc: %s | UA: %s", connection_hdr, accept_encoding, user_agent);
        ESP_LOGE(TAG, "  Resultado -> ESP_ERR: 0x%x (%s) | ERRNO: %d (%s)",
                 err, esp_err_to_name(err), saved_errno, strerror(saved_errno));
        ESP_LOGE(TAG, "  Classificacao Objetiva: %s", classificacao_neutra);
        return err;
    }

    ESP_LOGI(TAG, "[DASH SUCESSO 100%%] Transmitidos %zu B em %lu ms (%.2f KB/s) para IP %s | URI: %s",
             html_gz_len, (unsigned long)elapsed_ms, throughput_kbps, client_ip, req->uri);
    return ESP_OK;
}

void dashboard_register(httpd_handle_t server)
{
    if (server == NULL) {
        ESP_LOGE(TAG, "[DASH ERRO INIC] Handle server eh NULL no dashboard_register");
        return;
    }

    httpd_uri_t dash = {
        .uri = "/dash",
        .method = HTTP_GET,
        .handler = dashboard_handler,
        .user_ctx = NULL,
    };
    esp_err_t err = httpd_register_uri_handler(server, &dash);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Falha ao registrar /dash: %s", esp_err_to_name(err));
    }

    httpd_uri_t auth_html = {
        .uri = "/auth.html",
        .method = HTTP_GET,
        .handler = dashboard_handler,
        .user_ctx = NULL,
    };
    err = httpd_register_uri_handler(server, &auth_html);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Falha ao registrar /auth.html: %s", esp_err_to_name(err));
    }

    httpd_uri_t index_html = {
        .uri = "/index.html",
        .method = HTTP_GET,
        .handler = dashboard_handler,
        .user_ctx = NULL,
    };
    err = httpd_register_uri_handler(server, &index_html);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Falha ao registrar /index.html: %s", esp_err_to_name(err));
    }

    ESP_LOGI(TAG, "Dashboard gzipped registrada com sucesso em /dash, /auth.html e /index.html");
}

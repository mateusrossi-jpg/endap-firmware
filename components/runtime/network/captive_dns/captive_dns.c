#include "captive_dns.h"

#include "lwip/sockets.h"
#include "esp_log.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <stdbool.h>
#include <string.h>

#define TAG "CAPTIVE_DNS"

#define DNS_PORT 53
#define DNS_BUF 512

static bool dns_started = false;
static TaskHandle_t dns_task_handle = NULL;
static int dns_sock = -1;

static void dns_task(void *pvParameters)
{
    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);

    if (sock < 0)
    {
        ESP_LOGE(TAG, "Socket error");
        dns_started = false;
        dns_task_handle = NULL;
        vTaskDelete(NULL);
        return;
    }

    dns_sock = sock;

    /* 🔥 melhora robustez */
    int opt = 1;
    setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in server_addr = {
        .sin_family = AF_INET,
        .sin_port = htons(DNS_PORT),
        .sin_addr.s_addr = INADDR_ANY
    };

    if (bind(sock, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0)
    {
        ESP_LOGE(TAG, "Bind error");
        close(sock);
        dns_sock = -1;
        dns_started = false;
        dns_task_handle = NULL;
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "DNS Captive iniciado");

    uint8_t rx_buffer[DNS_BUF];
    uint8_t response[DNS_BUF];

    while (dns_started)
    {
        struct sockaddr_in client;
        socklen_t len = sizeof(client);

        int r = recvfrom(sock, rx_buffer, sizeof(rx_buffer), 0,
                         (struct sockaddr *)&client, &len);

        if (!dns_started)
            break;

        if (r < 12)
            continue;

        /* copia request */
        memcpy(response, rx_buffer, r);

        /* 🔥 flags DNS corretas */
        response[2] = 0x81;
        response[3] = 0x80;

        /* respostas = 1 */
        response[6] = 0x00;
        response[7] = 0x01;

        int idx = r;

        /* 🔥 evita overflow */
        if (idx + 16 >= DNS_BUF)
            continue;

        /* pointer para nome */
        response[idx++] = 0xC0;
        response[idx++] = 0x0C;

        /* TYPE A */
        response[idx++] = 0x00;
        response[idx++] = 0x01;

        /* CLASS IN */
        response[idx++] = 0x00;
        response[idx++] = 0x01;

        /* TTL */
        response[idx++] = 0x00;
        response[idx++] = 0x00;
        response[idx++] = 0x00;
        response[idx++] = 0x3C;

        /* tamanho IP */
        response[idx++] = 0x00;
        response[idx++] = 0x04;

        /* 🔥 SEMPRE REDIRECIONA PARA ESP */
        response[idx++] = 192;
        response[idx++] = 168;
        response[idx++] = 4;
        response[idx++] = 1;

        sendto(sock, response, idx, 0,
               (struct sockaddr *)&client, sizeof(client));
    }

    if (sock >= 0)
        close(sock);

    dns_sock = -1;
    dns_task_handle = NULL;
    vTaskDelete(NULL);
}

void captive_dns_start(void)
{
    if (dns_started)
        return;

    dns_started = true;

    if (xTaskCreate(dns_task, "captive_dns", 4096, NULL, 5, &dns_task_handle) != pdPASS)
    {
        dns_started = false;
        dns_task_handle = NULL;
        ESP_LOGE(TAG, "Falha ao iniciar task de captive DNS");
    }
}

void captive_dns_stop(void)
{
    if (!dns_started)
        return;

    dns_started = false;

    if (dns_sock >= 0)
        shutdown(dns_sock, SHUT_RDWR);
}

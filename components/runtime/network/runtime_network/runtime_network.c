#include "runtime_network.h"
#include "protocol.h"

#include <string.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <stdbool.h>
#include <unistd.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_log.h"

#define TAG "RT_NET"
#define PORT 5000

static int sock = -1;
static struct sockaddr_in dest_addr;
static bool runtime_network_started = false;

/* ============================================================
   RX TASK
============================================================ */

static void runtime_network_rx_task(void *arg)
{
    uint8_t rx_buffer[256];

    struct sockaddr_in source_addr;
    socklen_t socklen = sizeof(source_addr);

    while (1)
    {
        int len = recvfrom(sock,
                           rx_buffer,
                           sizeof(rx_buffer),
                           0,
                           (struct sockaddr *)&source_addr,
                           &socklen);

        if (len > 0)
        {
            ESP_LOGI(TAG, "RX %d bytes", len);

            protocol_process_frame(rx_buffer, len);
        }
    }
}

/* ============================================================
   INIT
============================================================ */

void runtime_network_init(void)
{
    if (runtime_network_started)
        return;

    sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);

    if (sock < 0)
    {
        ESP_LOGE(TAG, "Erro ao criar socket");
        return;
    }

    // permitir broadcast
    int broadcast = 1;
    setsockopt(sock, SOL_SOCKET, SO_BROADCAST, &broadcast, sizeof(broadcast));

    // BIND (ESSENCIAL pra receber)
    struct sockaddr_in local_addr = {0};
    local_addr.sin_family = AF_INET;
    local_addr.sin_port = htons(PORT);
    local_addr.sin_addr.s_addr = htonl(INADDR_ANY);

    if (bind(sock, (struct sockaddr *)&local_addr, sizeof(local_addr)) < 0)
    {
        ESP_LOGE(TAG, "Erro no bind");
        close(sock);
        sock = -1;
        return;
    }

    // destino broadcast
    dest_addr.sin_family = AF_INET;
    dest_addr.sin_port = htons(PORT);
    dest_addr.sin_addr.s_addr = inet_addr("255.255.255.255");

    ESP_LOGI(TAG, "Runtime network iniciado (UDP)");
    runtime_network_started = true;

    // criar task RX
    xTaskCreatePinnedToCore(
        runtime_network_rx_task,
        "rt_net_rx",
        4096,
        NULL,
        5,
        NULL,
        0
    );
}

/* ============================================================
   SEND
============================================================ */

void runtime_network_send(uint8_t *data, uint16_t len)
{
    if (sock < 0) return;

    sendto(sock,
           data,
           len,
           0,
           (struct sockaddr *)&dest_addr,
           sizeof(dest_addr));
}

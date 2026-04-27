#include "node_identity.h"

#include "esp_err.h"
#include "esp_log.h"
#include "esp_mac.h"

#include <inttypes.h>

#define TAG "NODE_ID"

static uint32_t node_id = 0;

/* ============================================================
   HASH SIMPLES DO MAC
============================================================ */

static uint32_t mac_to_id(uint8_t mac[6])
{
    uint32_t id = 0;

    id |= ((uint32_t)mac[2] << 24);
    id |= ((uint32_t)mac[3] << 16);
    id |= ((uint32_t)mac[4] << 8);
    id |= ((uint32_t)mac[5]);

    return id;
}

/* ============================================================
   INIT
============================================================ */

void node_identity_init(void)
{
    uint8_t mac[6] = {0};
    esp_err_t err;

    err = esp_read_mac(mac, ESP_MAC_WIFI_STA);
    if (err != ESP_OK)
    {
        node_id = 0;
        ESP_LOGE(TAG, "Falha ao ler MAC base do node: %s", esp_err_to_name(err));
        return;
    }

    node_id = mac_to_id(mac);

    ESP_LOGI(TAG, "Node ID: %" PRIu32 " (MAC %02X:%02X:%02X:%02X:%02X:%02X)",
        node_id,
        mac[0], mac[1], mac[2],
        mac[3], mac[4], mac[5]);
}

/* ============================================================
   GET
============================================================ */

uint32_t node_identity_get(void)
{
    return node_id;
}

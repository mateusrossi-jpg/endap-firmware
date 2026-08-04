#include "cluster_transport_now.h"
#include "esp_now.h"
#include "esp_wifi.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_timer.h"
#include <string.h>

static const char *TAG = "CLUSTER_NOW";
static uint32_t current_node_id = 0;
static cluster_transport_heartbeat_cb_t now_heartbeat_cb = NULL;
static cluster_transport_frame_cb_t now_frame_cb = NULL;
static bool now_initialized = false;
static cluster_now_stats_t now_stats = {0};

// Broadcast MAC address
static const uint8_t broadcast_mac[ESP_NOW_ETH_ALEN] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };

static void cluster_now_recv_cb(const esp_now_recv_info_t *recv_info, const uint8_t *data, int len)
{
    if (!data || len == 0) return;

    now_stats.rx_packets++;
    now_stats.last_rx_ms = (uint32_t)(esp_timer_get_time() / 1000ULL);

    if (recv_info && recv_info->src_addr) {
        if (!esp_now_is_peer_exist(recv_info->src_addr)) {
            esp_now_peer_info_t peer_info = {0};
            peer_info.channel = 0;
            peer_info.ifidx = WIFI_IF_STA; // Or APSTA
            peer_info.encrypt = false;
            memcpy(peer_info.peer_addr, recv_info->src_addr, ESP_NOW_ETH_ALEN);
            if (esp_now_add_peer(&peer_info) == ESP_OK) {
                now_stats.peers_count++;
                ESP_LOGI(TAG, "Peer added MAC=" MACSTR, MAC2STR(recv_info->src_addr));
            }
        }
    }

    ESP_LOGI(TAG, "Packet received size=%d", len);

    // We can peek at the first few bytes to determine if it's a heartbeat or a frame.
    if (len >= sizeof(endap_heartbeat_t))
    {
        endap_heartbeat_t *hb = (endap_heartbeat_t *)data;
        if (hb->magic == 0x454E4450)
        {
            ESP_LOGI(TAG, "RX heartbeat seq=%" PRIu32 " from=node=%" PRIu32, hb->boot_counter, hb->node_id);
            if (now_heartbeat_cb)
            {
                uint32_t sender_id = hb->node_id;
                if (sender_id != current_node_id)
                {
                    cluster_transport_heartbeat_t heartbeat = {
                        .node_id = sender_id,
                        .timestamp_ms = hb->uptime_seconds * 1000U,
                        .source_ip = 0U, // Not applicable for ESP-NOW
                        .source_transport = (uint8_t)CLUSTER_TRANSPORT_WIFI_NOW,
                        .device_profile = hb->node_type,
                        .uptime_seconds = hb->uptime_seconds,
                        .flags = hb->state
                    };
                    now_heartbeat_cb(&heartbeat);
                }
            }
            return;
        }
    }

    if (now_frame_cb)
    {
        now_frame_cb(data, (uint16_t)len);
    }
}

bool cluster_transport_now_init(uint32_t self_node_id)
{
    if (now_initialized) return true;

    current_node_id = self_node_id;
    memset(&now_stats, 0, sizeof(now_stats));

    esp_err_t err = esp_now_init();
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "Falha ao inicializar ESP-NOW: %s", esp_err_to_name(err));
        return false;
    }

    err = esp_now_register_recv_cb(cluster_now_recv_cb);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "Falha ao registrar callback de rx ESP-NOW: %s", esp_err_to_name(err));
        esp_now_deinit();
        return false;
    }

    esp_now_peer_info_t peer_info = {0};
    peer_info.channel = 0; // use current channel
    peer_info.ifidx = WIFI_IF_STA; // We will use STA even if APSTA mode
    peer_info.encrypt = false;
    memcpy(peer_info.peer_addr, broadcast_mac, ESP_NOW_ETH_ALEN);

    err = esp_now_add_peer(&peer_info);
    if (err != ESP_OK && err != ESP_ERR_ESPNOW_EXIST)
    {
        ESP_LOGE(TAG, "Falha ao adicionar peer de broadcast: %s", esp_err_to_name(err));
        esp_now_deinit();
        return false;
    }

    now_initialized = true;
    ESP_LOGI(TAG, "Transporte ESP-NOW inicializado");
    return true;
}

void cluster_transport_now_deinit(void)
{
    if (!now_initialized) return;
    esp_now_del_peer(broadcast_mac);
    esp_now_deinit();
    now_initialized = false;
    ESP_LOGI(TAG, "Transporte ESP-NOW desativado");
}

bool cluster_transport_now_send(const uint8_t *data, size_t len)
{
    if (!now_initialized || !data || len == 0) return false;

    // We can check if it's a heartbeat to log seq=x, but we might not have access to boot_counter directly without casting.
    if (len >= sizeof(endap_heartbeat_t)) {
        endap_heartbeat_t *hb = (endap_heartbeat_t *)data;
        if (hb->magic == 0x454E4450) {
            ESP_LOGI(TAG, "TX heartbeat seq=%" PRIu32, hb->boot_counter);
        }
    }

    ESP_LOGI(TAG, "Packet sent size=%zu", len);

    esp_err_t err = esp_now_send(broadcast_mac, data, len);
    if (err != ESP_OK)
    {
        ESP_LOGD(TAG, "Falha no envio ESP-NOW: %s", esp_err_to_name(err));
        now_stats.tx_errors++;
        return false;
    }

    now_stats.tx_packets++;
    now_stats.last_tx_ms = (uint32_t)(esp_timer_get_time() / 1000ULL);
    return true;
}

void cluster_transport_now_register_callbacks(cluster_transport_heartbeat_cb_t hb_cb, cluster_transport_frame_cb_t frame_cb)
{
    now_heartbeat_cb = hb_cb;
    now_frame_cb = frame_cb;
}

void cluster_transport_now_get_stats(cluster_now_stats_t *stats)
{
    if (stats) {
        *stats = now_stats;
    }
}

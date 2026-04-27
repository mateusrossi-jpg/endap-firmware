#include "cluster_transport.h"
#include "network_ready.h"
#include "device_profile.h"
#include "rs485_engine.h"

#include "esp_log.h"
#include "esp_timer.h"

#include "freertos/FreeRTOS.h"
#include "freertos/portmacro.h"
#include "freertos/task.h"

#include "lwip/inet.h"
#include "lwip/ip4_addr.h"
#include "lwip/netdb.h"
#include "lwip/sockets.h"

#include <inttypes.h>
#include <stdbool.h>
#include <string.h>
#include <unistd.h>

static const char *TAG = "CLUSTER_TRANS";

#define HEARTBEAT_MSG_TYPE 1U
#define CLUSTER_TRANSPORT_RX_STACK_WORDS 4096

typedef struct __attribute__((packed))
{
    uint8_t type;
    uint32_t node_id;
    uint32_t timestamp;
} cluster_transport_heartbeat_msg_v1_t;

typedef struct __attribute__((packed))
{
    uint8_t type;
    uint32_t node_id;
    uint32_t timestamp;
    uint8_t transport_type;
} cluster_transport_heartbeat_msg_t;

typedef struct __attribute__((packed))
{
    uint32_t node_id;
    uint32_t timestamp_ms;
    uint8_t transport_type;
} cluster_transport_rs485_heartbeat_payload_t;

static int discovery_sock = -1;
static int frame_sock = -1;
static struct sockaddr_in discovery_broadcast_addr;
static struct sockaddr_in frame_broadcast_addr;
static uint32_t self_node_id = 0;
static bool transport_started = false;
static bool network_callback_registered = false;
static bool udp_runtime_started = false;
static TaskHandle_t discovery_task_handle = NULL;
static TaskHandle_t frame_task_handle = NULL;
static cluster_transport_type_t active_type = CLUSTER_TRANSPORT_NONE;
static cluster_transport_heartbeat_cb_t heartbeat_callback = NULL;
static cluster_transport_frame_cb_t frame_callback = NULL;
static uint16_t rs485_msg_id_counter = 1U;
static portMUX_TYPE cluster_transport_lock = portMUX_INITIALIZER_UNLOCKED;

static bool cluster_transport_profile_allows(cluster_transport_type_t type);
static bool cluster_transport_start_udp_runtime(void);

static void cluster_transport_set_sockaddr(struct sockaddr_in *addr,
                                           uint16_t port,
                                           uint32_t ipv4_addr)
{
    if (!addr)
        return;

    memset(addr, 0, sizeof(*addr));
    addr->sin_family = AF_INET;
    addr->sin_port = htons(port);
    addr->sin_addr.s_addr = ipv4_addr;
}

static void cluster_transport_ip_to_text(uint32_t ipv4_addr, char *buf, size_t buf_size)
{
    ip4_addr_t ip = {0};

    if (!buf || buf_size == 0U)
        return;

    buf[0] = '\0';
    ip.addr = ipv4_addr;
    ip4addr_ntoa_r(&ip, buf, buf_size);
}

static uint16_t cluster_transport_next_rs485_msg_id(void)
{
    uint16_t current = rs485_msg_id_counter++;

    if (rs485_msg_id_counter == 0U)
        rs485_msg_id_counter = 1U;

    return current;
}

static bool cluster_transport_rs485_available(void)
{
    rs485_engine_metrics_t metrics = {0};

    rs485_engine_get_metrics(&metrics);
    return metrics.enabled;
}

static bool cluster_transport_rs485_rx_enabled(void)
{
    return cluster_transport_profile_allows(CLUSTER_TRANSPORT_RS485) &&
           cluster_transport_rs485_available();
}

static bool cluster_transport_type_uses_udp(cluster_transport_type_t type)
{
    return type == CLUSTER_TRANSPORT_WIFI_UDP ||
           type == CLUSTER_TRANSPORT_ETHERNET_UDP;
}

static void cluster_transport_on_rs485_frame(const rs485_frame_t *frame)
{
    if (!frame)
        return;

    if (!cluster_transport_rs485_rx_enabled())
        return;

    if (frame->type == RS485_FRAME_TYPE_CLUSTER_HEARTBEAT)
    {
        if (heartbeat_callback && frame->len >= sizeof(cluster_transport_rs485_heartbeat_payload_t))
        {
            cluster_transport_rs485_heartbeat_payload_t payload = {0};
            memcpy(&payload, frame->payload, sizeof(payload));

            if (payload.node_id == self_node_id)
                return;

            cluster_transport_heartbeat_t heartbeat = {
                .node_id = payload.node_id,
                .timestamp_ms = payload.timestamp_ms,
                .source_ip = 0U,
                .source_transport = payload.transport_type ?
                    payload.transport_type : (uint8_t)CLUSTER_TRANSPORT_RS485,
            };

            heartbeat_callback(&heartbeat);
        }

        return;
    }

    if (frame->type == RS485_FRAME_TYPE_CLUSTER_FRAME)
    {
        if (frame_callback)
            frame_callback(frame->payload, frame->len);

        return;
    }
}

static cluster_transport_type_t cluster_transport_type_for_link(network_ready_link_t link)
{
    if (link == NETWORK_READY_LINK_ETHERNET)
        return CLUSTER_TRANSPORT_ETHERNET_UDP;

    if (link == NETWORK_READY_LINK_WIFI_AP || link == NETWORK_READY_LINK_WIFI_STA)
        return CLUSTER_TRANSPORT_WIFI_UDP;

    return CLUSTER_TRANSPORT_NONE;
}

static bool cluster_transport_profile_allows(cluster_transport_type_t type)
{
    switch (type)
    {
        case CLUSTER_TRANSPORT_ETHERNET_UDP:
            return device_profile_transport_enabled(DEVICE_PROFILE_TRANSPORT_ETHERNET);
        case CLUSTER_TRANSPORT_WIFI_UDP:
            return device_profile_transport_enabled(DEVICE_PROFILE_TRANSPORT_WIFI);
        case CLUSTER_TRANSPORT_RS485:
            return device_profile_transport_enabled(DEVICE_PROFILE_TRANSPORT_RS485);
        case CLUSTER_TRANSPORT_NONE:
        default:
            return true;
    }
}

static uint8_t cluster_transport_current_source_code(void)
{
    return (uint8_t)cluster_transport_active_type();
}

static uint32_t cluster_transport_compute_broadcast_addr(uint32_t ip_addr, uint32_t netmask_addr)
{
    if (ip_addr == 0U || netmask_addr == 0U)
        return htonl(INADDR_BROADCAST);

    return (ip_addr & netmask_addr) | (~netmask_addr);
}

static int cluster_transport_make_udp_socket(uint16_t port)
{
    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
    int yes = 1;
    struct sockaddr_in local_addr = {0};

    if (sock < 0)
    {
        ESP_LOGE(TAG, "Falha ao criar socket UDP na porta %u", port);
        return -1;
    }

    setsockopt(sock, SOL_SOCKET, SO_BROADCAST, &yes, sizeof(yes));
    setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

    local_addr.sin_family = AF_INET;
    local_addr.sin_port = htons(port);
    local_addr.sin_addr.s_addr = htonl(INADDR_ANY);

    if (bind(sock, (struct sockaddr *)&local_addr, sizeof(local_addr)) < 0)
    {
        ESP_LOGE(TAG, "Falha no bind UDP da porta %u", port);
        close(sock);
        return -1;
    }

    return sock;
}

static void cluster_transport_close_all(void)
{
    if (discovery_sock >= 0)
    {
        close(discovery_sock);
        discovery_sock = -1;
    }

    if (frame_sock >= 0)
    {
        close(frame_sock);
        frame_sock = -1;
    }

    udp_runtime_started = false;
    discovery_task_handle = NULL;
    frame_task_handle = NULL;
}

static void cluster_transport_discovery_rx_task(void *arg)
{
    (void)arg;

    while (1)
    {
        uint8_t raw[sizeof(cluster_transport_heartbeat_msg_t)] = {0};
        struct sockaddr_in source_addr = {0};
        socklen_t addr_len = sizeof(source_addr);
        int len = recvfrom(discovery_sock,
                           raw,
                           sizeof(raw),
                           0,
                           (struct sockaddr *)&source_addr,
                           &addr_len);

        if (len != (int)sizeof(cluster_transport_heartbeat_msg_v1_t) &&
            len != (int)sizeof(cluster_transport_heartbeat_msg_t))
            continue;

        if (heartbeat_callback)
        {
            cluster_transport_heartbeat_t heartbeat = {0};

            if (len == (int)sizeof(cluster_transport_heartbeat_msg_t))
            {
                cluster_transport_heartbeat_msg_t msg = {0};
                memcpy(&msg, raw, sizeof(msg));
                if (msg.type != HEARTBEAT_MSG_TYPE || msg.node_id == self_node_id)
                    continue;
                heartbeat.node_id = msg.node_id;
                heartbeat.timestamp_ms = msg.timestamp;
                heartbeat.source_transport = msg.transport_type ?
                    msg.transport_type : (uint8_t)cluster_transport_active_type();
            }
            else
            {
                cluster_transport_heartbeat_msg_v1_t msg = {0};
                memcpy(&msg, raw, sizeof(msg));
                if (msg.type != HEARTBEAT_MSG_TYPE || msg.node_id == self_node_id)
                    continue;
                heartbeat.node_id = msg.node_id;
                heartbeat.timestamp_ms = msg.timestamp;
                heartbeat.source_transport = (uint8_t)cluster_transport_active_type();
            }

            heartbeat.source_ip = source_addr.sin_addr.s_addr;
            heartbeat_callback(&heartbeat);
        }
    }
}

static void cluster_transport_frame_rx_task(void *arg)
{
    (void)arg;

    while (1)
    {
        uint8_t rx_buffer[256];
        struct sockaddr_in source_addr = {0};
        socklen_t socklen = sizeof(source_addr);
        int len = recvfrom(frame_sock,
                           rx_buffer,
                           sizeof(rx_buffer),
                           0,
                           (struct sockaddr *)&source_addr,
                           &socklen);

        (void)source_addr;

        if (len <= 0)
            continue;

        if (frame_callback)
            frame_callback(rx_buffer, (uint16_t)len);
    }
}

static bool cluster_transport_start_udp_runtime(void)
{
    BaseType_t rx_ok;
    BaseType_t frame_ok;

    if (udp_runtime_started && discovery_sock >= 0 && frame_sock >= 0)
        return true;

    discovery_sock = cluster_transport_make_udp_socket(CLUSTER_TRANSPORT_DISCOVERY_PORT);
    if (discovery_sock < 0)
        return false;

    frame_sock = cluster_transport_make_udp_socket(CLUSTER_TRANSPORT_FRAME_PORT);
    if (frame_sock < 0)
    {
        cluster_transport_close_all();
        return false;
    }

    cluster_transport_set_sockaddr(
        &discovery_broadcast_addr,
        CLUSTER_TRANSPORT_DISCOVERY_PORT,
        htonl(INADDR_BROADCAST));
    cluster_transport_set_sockaddr(
        &frame_broadcast_addr,
        CLUSTER_TRANSPORT_FRAME_PORT,
        htonl(INADDR_BROADCAST));

    rx_ok = xTaskCreatePinnedToCore(
        cluster_transport_discovery_rx_task,
        "cluster_tr_rx",
        CLUSTER_TRANSPORT_RX_STACK_WORDS,
        NULL,
        5,
        &discovery_task_handle,
        0);

    frame_ok = xTaskCreatePinnedToCore(
        cluster_transport_frame_rx_task,
        "cluster_fr_rx",
        CLUSTER_TRANSPORT_RX_STACK_WORDS,
        NULL,
        5,
        &frame_task_handle,
        0);

    if (rx_ok != pdPASS || frame_ok != pdPASS)
    {
        ESP_LOGE(TAG, "Falha ao iniciar tasks do transporte de cluster");
        cluster_transport_close_all();
        return false;
    }

    udp_runtime_started = true;
    ESP_LOGI(TAG, "Cluster UDP runtime inicializado");
    return true;
}

static void cluster_transport_apply_network_snapshot(const network_ready_snapshot_t *snapshot,
                                                     cluster_transport_type_t fallback_type)
{
    cluster_transport_type_t next_type = CLUSTER_TRANSPORT_NONE;
    cluster_transport_type_t previous_type;
    uint32_t next_broadcast_addr = htonl(INADDR_BROADCAST);
    uint32_t previous_broadcast_addr;
    bool changed = false;
    char ip_text[20];
    char broadcast_text[20];
    cluster_transport_type_t preferred_ip_type = CLUSTER_TRANSPORT_NONE;

    if (snapshot && snapshot->ready)
    {
        cluster_transport_type_t link_type = cluster_transport_type_for_link(snapshot->active_link);

        if (cluster_transport_profile_allows(link_type))
            preferred_ip_type = link_type;

        if (preferred_ip_type != CLUSTER_TRANSPORT_NONE)
        {
            if (cluster_transport_start_udp_runtime())
            {
                next_type = preferred_ip_type;
                next_broadcast_addr = cluster_transport_compute_broadcast_addr(
                    snapshot->active_ip_addr,
                    snapshot->active_netmask_addr);
            }
            else if (cluster_transport_profile_allows(CLUSTER_TRANSPORT_RS485) &&
                     cluster_transport_rs485_available())
            {
                ESP_LOGW(TAG, "UDP indisponivel; mantendo cluster em RS485");
                next_type = CLUSTER_TRANSPORT_RS485;
            }
        }
        else if (cluster_transport_profile_allows(CLUSTER_TRANSPORT_RS485) &&
                 cluster_transport_rs485_available())
        {
            next_type = CLUSTER_TRANSPORT_RS485;
        }
        else if (cluster_transport_profile_allows(fallback_type))
        {
            if (!cluster_transport_type_uses_udp(fallback_type) || cluster_transport_start_udp_runtime())
                next_type = fallback_type;
        }
    }
    else if (cluster_transport_profile_allows(CLUSTER_TRANSPORT_RS485) &&
             cluster_transport_rs485_available())
    {
        next_type = CLUSTER_TRANSPORT_RS485;
    }
    else if (cluster_transport_profile_allows(fallback_type))
    {
        if (!cluster_transport_type_uses_udp(fallback_type) || cluster_transport_start_udp_runtime())
            next_type = fallback_type;
    }

    portENTER_CRITICAL(&cluster_transport_lock);
    previous_type = active_type;
    previous_broadcast_addr = discovery_broadcast_addr.sin_addr.s_addr;
    cluster_transport_set_sockaddr(
        &discovery_broadcast_addr,
        CLUSTER_TRANSPORT_DISCOVERY_PORT,
        next_broadcast_addr);
    cluster_transport_set_sockaddr(
        &frame_broadcast_addr,
        CLUSTER_TRANSPORT_FRAME_PORT,
        next_broadcast_addr);
    active_type = next_type;
    changed = previous_type != next_type || previous_broadcast_addr != next_broadcast_addr;
    portEXIT_CRITICAL(&cluster_transport_lock);

    if (!changed)
        return;

    cluster_transport_ip_to_text(snapshot ? snapshot->active_ip_addr : 0U, ip_text, sizeof(ip_text));
    cluster_transport_ip_to_text(next_broadcast_addr, broadcast_text, sizeof(broadcast_text));

    ESP_LOGI(TAG,
             "Cluster route atualizado: link=%s type=%s ip=%s broadcast=%s",
             snapshot ? network_ready_link_name(snapshot->active_link) : "none",
             cluster_transport_active_name(),
             ip_text[0] ? ip_text : "0.0.0.0",
             broadcast_text[0] ? broadcast_text : "255.255.255.255");
}

static void cluster_transport_on_network_ready(const network_ready_snapshot_t *snapshot, void *ctx)
{
    cluster_transport_type_t fallback_type;

    (void)ctx;

    if (!transport_started)
        return;

    fallback_type = cluster_transport_rs485_available() ?
        CLUSTER_TRANSPORT_RS485 : cluster_transport_active_type();
    cluster_transport_apply_network_snapshot(snapshot, fallback_type);
}

void cluster_transport_set_active_type(cluster_transport_type_t type)
{
    portENTER_CRITICAL(&cluster_transport_lock);
    active_type = type;
    portEXIT_CRITICAL(&cluster_transport_lock);
}

bool cluster_transport_start(uint32_t node_id, cluster_transport_type_t type)
{
    network_ready_snapshot_t snapshot = {0};

    if (transport_started)
    {
        self_node_id = node_id;
        rs485_engine_register_external_frame_callback(cluster_transport_on_rs485_frame);
        network_ready_get_snapshot(&snapshot);
        cluster_transport_apply_network_snapshot(&snapshot, type);
        return true;
    }

    self_node_id = node_id;
    rs485_engine_register_external_frame_callback(cluster_transport_on_rs485_frame);

    cluster_transport_set_sockaddr(
        &discovery_broadcast_addr,
        CLUSTER_TRANSPORT_DISCOVERY_PORT,
        htonl(INADDR_BROADCAST));
    cluster_transport_set_sockaddr(
        &frame_broadcast_addr,
        CLUSTER_TRANSPORT_FRAME_PORT,
        htonl(INADDR_BROADCAST));

    if (!network_callback_registered)
    {
        network_callback_registered = network_ready_register_callback(
            cluster_transport_on_network_ready,
            NULL);

        if (!network_callback_registered)
            ESP_LOGW(TAG, "Falha ao registrar callback de network_ready no cluster");
    }

    transport_started = true;
    network_ready_get_snapshot(&snapshot);
    cluster_transport_apply_network_snapshot(&snapshot, type);

    if (cluster_transport_type_uses_udp(type) &&
        cluster_transport_active_type() == CLUSTER_TRANSPORT_NONE &&
        !cluster_transport_rs485_available())
    {
        ESP_LOGW(TAG, "Cluster aguardando pilha IP para ativar transporte UDP");
    }

    ESP_LOGI(TAG,
             "Cluster transport ativo (%s, node=%" PRIu32 ")",
             cluster_transport_active_name(),
             self_node_id);
    return true;
}

bool cluster_transport_is_ready(void)
{
    bool ready;

    portENTER_CRITICAL(&cluster_transport_lock);
    ready = transport_started && active_type != CLUSTER_TRANSPORT_NONE;
    if (active_type == CLUSTER_TRANSPORT_RS485)
        ready = ready && cluster_transport_rs485_available();
    else
        ready = ready && discovery_sock >= 0 && frame_sock >= 0;
    portEXIT_CRITICAL(&cluster_transport_lock);

    return ready;
}

cluster_transport_type_t cluster_transport_active_type(void)
{
    cluster_transport_type_t type;

    portENTER_CRITICAL(&cluster_transport_lock);
    type = active_type;
    portEXIT_CRITICAL(&cluster_transport_lock);

    return type;
}

const char *cluster_transport_name(cluster_transport_type_t type)
{
    switch (type)
    {
        case CLUSTER_TRANSPORT_WIFI_UDP:
            return "wifi-udp";
        case CLUSTER_TRANSPORT_ETHERNET_UDP:
            return "ethernet-udp";
        case CLUSTER_TRANSPORT_RS485:
            return "rs485";
        case CLUSTER_TRANSPORT_NONE:
        default:
            return "none";
    }
}

const char *cluster_transport_active_name(void)
{
    return cluster_transport_name(cluster_transport_active_type());
}

void cluster_transport_register_heartbeat_callback(cluster_transport_heartbeat_cb_t cb)
{
    heartbeat_callback = cb;
}

void cluster_transport_register_frame_callback(cluster_transport_frame_cb_t cb)
{
    frame_callback = cb;
}

bool cluster_transport_send_heartbeat(void)
{
    cluster_transport_type_t type;
    cluster_transport_heartbeat_msg_t msg = {
        .type = HEARTBEAT_MSG_TYPE,
        .node_id = self_node_id,
        .timestamp = (uint32_t)(esp_timer_get_time() / 1000ULL),
        .transport_type = cluster_transport_current_source_code(),
    };
    struct sockaddr_in dest_addr;
    int sock;

    if (!cluster_transport_is_ready())
        return false;

    type = cluster_transport_active_type();

    if (type == CLUSTER_TRANSPORT_RS485)
    {
        cluster_transport_rs485_heartbeat_payload_t payload = {
            .node_id = self_node_id,
            .timestamp_ms = (uint32_t)(esp_timer_get_time() / 1000ULL),
            .transport_type = (uint8_t)CLUSTER_TRANSPORT_RS485,
        };
        rs485_frame_t frame = {
            .node = (uint8_t)self_node_id,
            .msg_id = cluster_transport_next_rs485_msg_id(),
            .type = RS485_FRAME_TYPE_CLUSTER_HEARTBEAT,
            .len = (uint8_t)sizeof(payload),
        };

        memcpy(frame.payload, &payload, sizeof(payload));
        rs485_engine_send(&frame);
        return true;
    }

    portENTER_CRITICAL(&cluster_transport_lock);
    sock = discovery_sock;
    dest_addr = discovery_broadcast_addr;
    portEXIT_CRITICAL(&cluster_transport_lock);

    return sendto(sock,
                  &msg,
                  sizeof(msg),
                  0,
                  (struct sockaddr *)&dest_addr,
                  sizeof(dest_addr)) == (int)sizeof(msg);
}

bool cluster_transport_broadcast_frame(const uint8_t *data, uint16_t len)
{
    cluster_transport_type_t type;
    struct sockaddr_in dest_addr;
    int sock;

    if (!cluster_transport_is_ready() || !data || len == 0U)
        return false;

    type = cluster_transport_active_type();

    if (type == CLUSTER_TRANSPORT_RS485)
    {
        rs485_frame_t frame = {
            .node = (uint8_t)self_node_id,
            .msg_id = cluster_transport_next_rs485_msg_id(),
            .type = RS485_FRAME_TYPE_CLUSTER_FRAME,
            .len = (uint8_t)len,
        };

        if (len > RS485_FRAME_MAX_PAYLOAD)
            return false;

        memcpy(frame.payload, data, len);
        rs485_engine_send(&frame);
        return true;
    }

    portENTER_CRITICAL(&cluster_transport_lock);
    sock = frame_sock;
    dest_addr = frame_broadcast_addr;
    portEXIT_CRITICAL(&cluster_transport_lock);

    return sendto(sock,
                  data,
                  len,
                  0,
                  (struct sockaddr *)&dest_addr,
                  sizeof(dest_addr)) == (int)len;
}

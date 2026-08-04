#include "cluster_transport.h"
#include "cluster_transport_now.h"
#include "cluster_transport_mesh.h"
#include "network_ready.h"
#include "device_profile.h"
#include "rs485_engine.h"
#include "system_diag.h"

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
        if (heartbeat_callback && frame->len >= sizeof(endap_heartbeat_t))
        {
            endap_heartbeat_t hb = {0};
            memcpy(&hb, frame->payload, sizeof(hb));

            if (hb.magic != 0x454E4450 || hb.node_id == self_node_id)
                return;

            cluster_transport_heartbeat_t heartbeat = {
                .node_id = hb.node_id,
                .timestamp_ms = hb.uptime_seconds * 1000U,
                .source_ip = 0U,
                .source_transport = (uint8_t)CLUSTER_TRANSPORT_RS485,
                .device_profile = hb.node_type,
                .uptime_seconds = hb.uptime_seconds,
                .flags = hb.state
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
        case CLUSTER_TRANSPORT_WIFI_NOW:
            return device_profile_transport_enabled(DEVICE_PROFILE_TRANSPORT_ESPNOW);
        case CLUSTER_TRANSPORT_WIFI_MESH:
            return device_profile_transport_enabled(DEVICE_PROFILE_TRANSPORT_MESH);
        case CLUSTER_TRANSPORT_NONE:
        default:
            return true;
    }
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
        uint8_t raw[sizeof(endap_heartbeat_t)] = {0};
        struct sockaddr_in source_addr = {0};
        socklen_t addr_len = sizeof(source_addr);
        int len = recvfrom(discovery_sock,
                           raw,
                           sizeof(raw),
                           0,
                           (struct sockaddr *)&source_addr,
                           &addr_len);

        if (len < (int)sizeof(endap_heartbeat_t))
            continue;

        if (heartbeat_callback)
        {
            endap_heartbeat_t *msg = (endap_heartbeat_t *)raw;
            if (msg->magic != 0x454E4450)
                continue;

            uint32_t incoming_node_id = msg->node_id;
            if (incoming_node_id == self_node_id)
                continue;

            cluster_transport_heartbeat_t heartbeat = {
                .node_id = incoming_node_id,
                .timestamp_ms = (uint32_t)(esp_timer_get_time() / 1000ULL),
                .source_ip = source_addr.sin_addr.s_addr,
                .source_transport = (uint8_t)cluster_transport_active_type(),
                .device_profile = msg->node_type,
                .uptime_seconds = msg->uptime_seconds,
                .flags = msg->state
            };

            heartbeat_callback(&heartbeat);
        }
    }
}

#include "endap_onboarding.h"
#include "freertos/semphr.h"

static SemaphoreHandle_t remote_claim_sem = NULL;
static endap_remote_claim_msg_t remote_claim_response = {0};

static void cluster_transport_handle_claim_frame(const uint8_t *data, uint16_t len)
{
    if (!data || len < sizeof(endap_remote_claim_msg_t))
        return;

    const endap_remote_claim_msg_t *msg = (const endap_remote_claim_msg_t *)data;
    if (msg->magic != ENDAP_REMOTE_CLAIM_MAGIC)
        return;

    if (msg->msg_type == ENDAP_REMOTE_CLAIM_REQ)
    {
        // Se a mensagem for direcionada a este nó
        if (msg->target_node_id == self_node_id)
        {
            ESP_LOGI(TAG, "Claim remoto recebido do Gateway %" PRIu32 " para perfil %u", msg->gateway_id, msg->profile);
            esp_err_t err = endap_onboarding_claim((node_profile_t)msg->profile, msg->gateway_id, msg->node_name);

            // Responde a confirmacao
            endap_remote_claim_msg_t resp = {
                .magic = ENDAP_REMOTE_CLAIM_MAGIC,
                .msg_type = ENDAP_REMOTE_CLAIM_RESP,
                .profile = msg->profile,
                .status_code = (uint16_t)err,
                .target_node_id = self_node_id,
                .gateway_id = msg->gateway_id,
            };
            snprintf(resp.node_name, sizeof(resp.node_name), "%s", msg->node_name);
            cluster_transport_broadcast_frame((const uint8_t *)&resp, sizeof(resp));
        }
    }
    else if (msg->msg_type == ENDAP_REMOTE_CLAIM_RESP)
    {
        // Resposta recebida pelo Gateway
        if (msg->gateway_id == self_node_id && remote_claim_sem != NULL)
        {
            memcpy(&remote_claim_response, msg, sizeof(remote_claim_response));
            xSemaphoreGive(remote_claim_sem);
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

        cluster_transport_handle_claim_frame(rx_buffer, (uint16_t)len);

        if (frame_callback)
            frame_callback(rx_buffer, (uint16_t)len);
    }
}

bool cluster_transport_send_remote_claim(uint32_t target_node_id, uint8_t profile, uint32_t gateway_id, const char *node_name, uint32_t timeout_ms)
{
    if (remote_claim_sem == NULL) {
        remote_claim_sem = xSemaphoreCreateBinary();
        if (!remote_claim_sem) return false;
    }

    xSemaphoreTake(remote_claim_sem, 0); // Limpa sinal previo
    memset(&remote_claim_response, 0, sizeof(remote_claim_response));

    endap_remote_claim_msg_t req = {
        .magic = ENDAP_REMOTE_CLAIM_MAGIC,
        .msg_type = ENDAP_REMOTE_CLAIM_REQ,
        .profile = profile,
        .status_code = 0,
        .target_node_id = target_node_id,
        .gateway_id = gateway_id,
    };
    if (node_name) {
        snprintf(req.node_name, sizeof(req.node_name), "%s", node_name);
    }

    if (!cluster_transport_broadcast_frame((const uint8_t *)&req, sizeof(req))) {
        ESP_LOGE(TAG, "Falha ao enviar frame de claim remoto");
        return false;
    }

    TickType_t ticks = pdMS_TO_TICKS(timeout_ms ? timeout_ms : 3000U);
    if (xSemaphoreTake(remote_claim_sem, ticks) == pdTRUE) {
        return (remote_claim_response.status_code == 0);
    }

    ESP_LOGW(TAG, "Timeout aguardando resposta de claim do no %" PRIu32, target_node_id);
    return false;
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

static cluster_transport_type_t cluster_transport_select_best(const network_ready_snapshot_t *snapshot)
{
    // 1. Ethernet UDP
    if (snapshot && snapshot->ethernet_up && cluster_transport_profile_allows(CLUSTER_TRANSPORT_ETHERNET_UDP))
    {
        return CLUSTER_TRANSPORT_ETHERNET_UDP;
    }

    // 2. WiFi UDP / MESH / NOW
    if (snapshot && (snapshot->wifi_sta_up || snapshot->wifi_ap_up))
    {
        const device_network_profile_t *net = device_profile_network();
        if (net)
        {
            if (net->wifi_mode == DEVICE_PROFILE_WIFI_MODE_MESH && cluster_transport_profile_allows(CLUSTER_TRANSPORT_WIFI_MESH))
                return CLUSTER_TRANSPORT_WIFI_MESH;
            if (net->wifi_mode == DEVICE_PROFILE_WIFI_MODE_NOW && cluster_transport_profile_allows(CLUSTER_TRANSPORT_WIFI_NOW))
                return CLUSTER_TRANSPORT_WIFI_NOW;
        }
        if (cluster_transport_profile_allows(CLUSTER_TRANSPORT_WIFI_UDP))
            return CLUSTER_TRANSPORT_WIFI_UDP;
    }

    // 3. Fallback ESP-NOW
    if (cluster_transport_profile_allows(CLUSTER_TRANSPORT_WIFI_NOW))
    {
        return CLUSTER_TRANSPORT_WIFI_NOW;
    }

    // 4. Fallback RS485
    if (cluster_transport_profile_allows(CLUSTER_TRANSPORT_RS485) && cluster_transport_rs485_available())
    {
        return CLUSTER_TRANSPORT_RS485;
    }

    return CLUSTER_TRANSPORT_NONE;
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
    cluster_transport_type_t preferred_type = cluster_transport_select_best(snapshot);

    if (preferred_type != CLUSTER_TRANSPORT_NONE)
    {
        if (cluster_transport_type_uses_udp(preferred_type))
        {
            if (cluster_transport_start_udp_runtime())
            {
                next_type = preferred_type;
                if (snapshot)
                {
                    next_broadcast_addr = cluster_transport_compute_broadcast_addr(
                        snapshot->active_ip_addr,
                        snapshot->active_netmask_addr);
                }
            }
            else if (cluster_transport_profile_allows(CLUSTER_TRANSPORT_RS485) &&
                     cluster_transport_rs485_available())
            {
                ESP_LOGW(TAG, "UDP indisponivel; mantendo cluster em RS485");
                next_type = CLUSTER_TRANSPORT_RS485;
            }
        }
        else
        {
            next_type = preferred_type;
        }
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
             
    if (next_type == CLUSTER_TRANSPORT_WIFI_NOW)
    {
        cluster_transport_now_init(self_node_id);
    }
    else if (previous_type == CLUSTER_TRANSPORT_WIFI_NOW)
    {
        cluster_transport_now_deinit();
    }
    
    if (next_type == CLUSTER_TRANSPORT_WIFI_MESH)
    {
        cluster_transport_mesh_init(self_node_id);
    }
    else if (previous_type == CLUSTER_TRANSPORT_WIFI_MESH)
    {
        cluster_transport_mesh_deinit();
    }
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
        case CLUSTER_TRANSPORT_WIFI_MESH:
            return "wifi-mesh";
        case CLUSTER_TRANSPORT_WIFI_NOW:
            return "wifi-now";
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
    cluster_transport_now_register_callbacks(heartbeat_callback, frame_callback);
    cluster_transport_mesh_register_callbacks(heartbeat_callback, frame_callback);
}

void cluster_transport_register_frame_callback(cluster_transport_frame_cb_t cb)
{
    frame_callback = cb;
    cluster_transport_now_register_callbacks(heartbeat_callback, frame_callback);
    cluster_transport_mesh_register_callbacks(heartbeat_callback, frame_callback);
}

bool cluster_transport_send_heartbeat(void)
{
    cluster_transport_type_t type;
    endap_heartbeat_t msg = {0};
    
    msg.magic = ENDAP_CLUSTER_MAGIC;
    msg.node_id = self_node_id;
    msg.boot_counter = system_diag_get_boot_count();
    msg.uptime_seconds = (uint32_t)(esp_timer_get_time() / 1000000ULL);
    msg.node_type = 1; // Default
    msg.state = 1; // ONLINE

    struct sockaddr_in dest_addr;
    int sock;

    if (!cluster_transport_is_ready())
        return false;

    type = cluster_transport_active_type();

    if (type == CLUSTER_TRANSPORT_RS485)
    {
        rs485_frame_t frame = {
            .node = (uint8_t)self_node_id,
            .msg_id = cluster_transport_next_rs485_msg_id(),
            .type = RS485_FRAME_TYPE_CLUSTER_HEARTBEAT,
            .len = (uint8_t)sizeof(msg),
        };

        memcpy(frame.payload, &msg, sizeof(msg));
        rs485_engine_send(&frame);
        return true;
    }
    
    if (type == CLUSTER_TRANSPORT_WIFI_NOW)
    {
        return cluster_transport_now_send((const uint8_t*)&msg, sizeof(msg));
    }
    else if (type == CLUSTER_TRANSPORT_WIFI_MESH)
    {
        return cluster_transport_mesh_send((const uint8_t*)&msg, sizeof(msg));
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

    if (type == CLUSTER_TRANSPORT_WIFI_NOW)
    {
        return cluster_transport_now_send(data, len);
    }
    else if (type == CLUSTER_TRANSPORT_WIFI_MESH)
    {
        return cluster_transport_mesh_send(data, len);
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

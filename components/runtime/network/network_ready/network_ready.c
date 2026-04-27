#include "network_ready.h"

#include "esp_log.h"

#include "freertos/FreeRTOS.h"
#include "freertos/portmacro.h"

#include <string.h>

#define TAG "NET_READY"
#define NETWORK_READY_MAX_CALLBACKS 4

typedef struct
{
    network_ready_callback_t cb;
    void *ctx;
} network_ready_callback_entry_t;

typedef struct
{
    bool up;
    uint32_t ip_addr;
    uint32_t netmask_addr;
} network_ready_link_state_t;

static portMUX_TYPE network_ready_lock = portMUX_INITIALIZER_UNLOCKED;
static network_ready_link_state_t link_state[4];
static network_ready_snapshot_t current_snapshot;
static network_ready_callback_entry_t callback_entries[NETWORK_READY_MAX_CALLBACKS];

static network_ready_link_t network_ready_choose_active_link(void)
{
    if (link_state[NETWORK_READY_LINK_ETHERNET].up)
        return NETWORK_READY_LINK_ETHERNET;

    if (link_state[NETWORK_READY_LINK_WIFI_STA].up)
        return NETWORK_READY_LINK_WIFI_STA;

    if (link_state[NETWORK_READY_LINK_WIFI_AP].up)
        return NETWORK_READY_LINK_WIFI_AP;

    return NETWORK_READY_LINK_NONE;
}

static void network_ready_refresh_snapshot(void)
{
    current_snapshot.active_link = network_ready_choose_active_link();
    current_snapshot.ready = (current_snapshot.active_link != NETWORK_READY_LINK_NONE);
    current_snapshot.active_ip_addr = link_state[current_snapshot.active_link].ip_addr;
    current_snapshot.active_netmask_addr = link_state[current_snapshot.active_link].netmask_addr;
    current_snapshot.wifi_ap_up = link_state[NETWORK_READY_LINK_WIFI_AP].up;
    current_snapshot.wifi_sta_up = link_state[NETWORK_READY_LINK_WIFI_STA].up;
    current_snapshot.ethernet_up = link_state[NETWORK_READY_LINK_ETHERNET].up;
    current_snapshot.wifi_ap_ip_addr = link_state[NETWORK_READY_LINK_WIFI_AP].ip_addr;
    current_snapshot.wifi_ap_netmask_addr = link_state[NETWORK_READY_LINK_WIFI_AP].netmask_addr;
    current_snapshot.wifi_sta_ip_addr = link_state[NETWORK_READY_LINK_WIFI_STA].ip_addr;
    current_snapshot.wifi_sta_netmask_addr = link_state[NETWORK_READY_LINK_WIFI_STA].netmask_addr;
    current_snapshot.ethernet_ip_addr = link_state[NETWORK_READY_LINK_ETHERNET].ip_addr;
    current_snapshot.ethernet_netmask_addr = link_state[NETWORK_READY_LINK_ETHERNET].netmask_addr;
}

static bool network_ready_snapshot_equals(const network_ready_snapshot_t *a,
                                          const network_ready_snapshot_t *b)
{
    if (!a || !b)
        return false;

    return a->ready == b->ready &&
           a->active_link == b->active_link &&
           a->active_ip_addr == b->active_ip_addr &&
           a->active_netmask_addr == b->active_netmask_addr &&
           a->wifi_ap_up == b->wifi_ap_up &&
           a->wifi_sta_up == b->wifi_sta_up &&
           a->ethernet_up == b->ethernet_up &&
           a->wifi_ap_ip_addr == b->wifi_ap_ip_addr &&
           a->wifi_ap_netmask_addr == b->wifi_ap_netmask_addr &&
           a->wifi_sta_ip_addr == b->wifi_sta_ip_addr &&
           a->wifi_sta_netmask_addr == b->wifi_sta_netmask_addr &&
           a->ethernet_ip_addr == b->ethernet_ip_addr &&
           a->ethernet_netmask_addr == b->ethernet_netmask_addr;
}

static void network_ready_notify(const network_ready_snapshot_t *snapshot)
{
    network_ready_callback_entry_t callbacks[NETWORK_READY_MAX_CALLBACKS];

    if (!snapshot)
        return;

    portENTER_CRITICAL(&network_ready_lock);
    memcpy(callbacks, callback_entries, sizeof(callbacks));
    portEXIT_CRITICAL(&network_ready_lock);

    for (int i = 0; i < NETWORK_READY_MAX_CALLBACKS; i++)
    {
        if (callbacks[i].cb)
            callbacks[i].cb(snapshot, callbacks[i].ctx);
    }
}

void network_ready_init(void)
{
    portENTER_CRITICAL(&network_ready_lock);
    memset(link_state, 0, sizeof(link_state));
    memset(callback_entries, 0, sizeof(callback_entries));
    memset(&current_snapshot, 0, sizeof(current_snapshot));
    portEXIT_CRITICAL(&network_ready_lock);
}

bool network_ready_register_callback(network_ready_callback_t cb, void *ctx)
{
    if (!cb)
        return false;

    portENTER_CRITICAL(&network_ready_lock);

    for (int i = 0; i < NETWORK_READY_MAX_CALLBACKS; i++)
    {
        if (callback_entries[i].cb == cb && callback_entries[i].ctx == ctx)
        {
            portEXIT_CRITICAL(&network_ready_lock);
            return true;
        }
    }

    for (int i = 0; i < NETWORK_READY_MAX_CALLBACKS; i++)
    {
        if (!callback_entries[i].cb)
        {
            callback_entries[i].cb = cb;
            callback_entries[i].ctx = ctx;
            portEXIT_CRITICAL(&network_ready_lock);
            return true;
        }
    }

    portEXIT_CRITICAL(&network_ready_lock);
    ESP_LOGW(TAG, "Tabela de callbacks cheia");
    return false;
}

bool network_ready_publish_up(network_ready_link_t link, uint32_t ip_addr, uint32_t netmask_addr)
{
    network_ready_snapshot_t previous;
    network_ready_snapshot_t next;

    if (link <= NETWORK_READY_LINK_NONE || link > NETWORK_READY_LINK_ETHERNET)
        return false;

    portENTER_CRITICAL(&network_ready_lock);
    previous = current_snapshot;
    link_state[link].up = true;
    link_state[link].ip_addr = ip_addr;
    link_state[link].netmask_addr = netmask_addr;
    network_ready_refresh_snapshot();
    next = current_snapshot;
    portEXIT_CRITICAL(&network_ready_lock);

    if (!network_ready_snapshot_equals(&previous, &next))
    {
        ESP_LOGI(TAG, "Link ativo: %s", network_ready_link_name(next.active_link));
        network_ready_notify(&next);
    }

    return true;
}

bool network_ready_publish_down(network_ready_link_t link)
{
    network_ready_snapshot_t previous;
    network_ready_snapshot_t next;

    if (link <= NETWORK_READY_LINK_NONE || link > NETWORK_READY_LINK_ETHERNET)
        return false;

    portENTER_CRITICAL(&network_ready_lock);
    previous = current_snapshot;
    link_state[link].up = false;
    link_state[link].ip_addr = 0U;
    link_state[link].netmask_addr = 0U;
    network_ready_refresh_snapshot();
    next = current_snapshot;
    portEXIT_CRITICAL(&network_ready_lock);

    if (!network_ready_snapshot_equals(&previous, &next))
    {
        ESP_LOGI(TAG, "Link ativo: %s", network_ready_link_name(next.active_link));
        network_ready_notify(&next);
    }

    return true;
}

bool network_ready_is_up(void)
{
    bool ready;

    portENTER_CRITICAL(&network_ready_lock);
    ready = current_snapshot.ready;
    portEXIT_CRITICAL(&network_ready_lock);

    return ready;
}

network_ready_link_t network_ready_active_link(void)
{
    network_ready_link_t active_link;

    portENTER_CRITICAL(&network_ready_lock);
    active_link = current_snapshot.active_link;
    portEXIT_CRITICAL(&network_ready_lock);

    return active_link;
}

const char *network_ready_active_name(void)
{
    return network_ready_link_name(network_ready_active_link());
}

const char *network_ready_link_name(network_ready_link_t link)
{
    switch (link)
    {
        case NETWORK_READY_LINK_WIFI_AP:
            return "wifi-ap";
        case NETWORK_READY_LINK_WIFI_STA:
            return "wifi-sta";
        case NETWORK_READY_LINK_ETHERNET:
            return "ethernet";
        case NETWORK_READY_LINK_NONE:
        default:
            return "none";
    }
}

void network_ready_get_snapshot(network_ready_snapshot_t *out_snapshot)
{
    if (!out_snapshot)
        return;

    portENTER_CRITICAL(&network_ready_lock);
    *out_snapshot = current_snapshot;
    portEXIT_CRITICAL(&network_ready_lock);
}

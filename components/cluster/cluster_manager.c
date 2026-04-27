#include "cluster_manager.h"
#include "cluster_events.h"
#include "cluster_metrics.h"

#include "esp_log.h"
#include "esp_timer.h"

#include "freertos/FreeRTOS.h"
#include "freertos/portmacro.h"

#include <stdbool.h>
#include <string.h>
#include <inttypes.h>

static const char *TAG = "CLUSTER_MGR";

#define HEARTBEAT_TIMEOUT_MS 3000
#define SUSPECT_TIMEOUT_MS   6000
#define CLUSTER_MANAGER_TICK_MS 2000
#define CLUSTER_MANAGER_MAX_PENDING_EVENTS MAX_NODES

static cluster_node_t nodes[MAX_NODES];
static bool cluster_manager_started = false;
static uint32_t self_node_id = 0;
static uint32_t cluster_manager_last_run_ms = 0;
static portMUX_TYPE cluster_manager_lock = portMUX_INITIALIZER_UNLOCKED;

/* buffers estáticos para evitar pressão na stack da task */
static cluster_node_t cluster_manager_snapshot_buf[MAX_NODES];
static cluster_event_t cluster_manager_events_buf[CLUSTER_MANAGER_MAX_PENDING_EVENTS];

/* ============================================================
   TIME
============================================================ */

static inline uint32_t now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000ULL);
}

static cluster_node_state_t cluster_manager_state_from_age(uint32_t age_ms)
{
    if (age_ms > SUSPECT_TIMEOUT_MS)
        return CLUSTER_NODE_OFFLINE;

    if (age_ms > HEARTBEAT_TIMEOUT_MS)
        return CLUSTER_NODE_SUSPECT;

    return CLUSTER_NODE_ONLINE;
}

/* ============================================================
   NODE ACCESS
============================================================ */

static cluster_node_t *find_node(uint32_t node_id)
{
    for (int i = 0; i < MAX_NODES; i++)
    {
        if (nodes[i].node_id == node_id)
            return &nodes[i];
    }

    return NULL;
}

static cluster_node_t *allocate_node(void)
{
    for (int i = 0; i < MAX_NODES; i++)
    {
        if (nodes[i].node_id == 0)
            return &nodes[i];
    }

    return NULL;
}

static cluster_node_t *get_or_create_node(uint32_t node_id)
{
    cluster_node_t *node = find_node(node_id);

    if (node)
        return node;

    node = allocate_node();
    if (!node)
        return NULL;

    memset(node, 0, sizeof(*node));
    node->node_id = node_id;
    node->state = CLUSTER_NODE_OFFLINE;
    return node;
}

/* ============================================================
   HEALTH
============================================================ */

static uint8_t calculate_health(const cluster_node_t *node)
{
    uint32_t penalty;

    if (!node || node->state == CLUSTER_NODE_OFFLINE)
        return 0;

    penalty = node->missed_heartbeats * 10U;

    if (penalty > 100U)
        penalty = 100U;

    return (uint8_t)(100U - penalty);
}

/* ============================================================
   SNAPSHOT HELPERS
============================================================ */

static void cluster_manager_copy_nodes_locked(cluster_node_t *out_nodes)
{
    if (!out_nodes)
        return;

    memcpy(out_nodes, nodes, sizeof(nodes));
}

static uint32_t cluster_manager_get_master_from_snapshot(const cluster_node_t *snapshot)
{
    uint32_t master = 0;

    if (!snapshot)
        return 0;

    for (int i = 0; i < MAX_NODES; i++)
    {
        if (snapshot[i].node_id == 0)
            continue;

        if (snapshot[i].state != CLUSTER_NODE_ONLINE)
            continue;

        if (master == 0 || snapshot[i].node_id < master)
            master = snapshot[i].node_id;
    }

    return master;
}

static int cluster_manager_refresh_nodes_locked(cluster_event_t *events, int max_events)
{
    int event_count = 0;
    uint32_t now = now_ms();

    for (int i = 0; i < MAX_NODES; i++)
    {
        cluster_node_t *node = &nodes[i];
        cluster_node_state_t old_state;

        if (node->node_id == 0 || node->last_seen_ms == 0)
            continue;

        old_state = node->state;

        if (node->node_id == self_node_id)
        {
            node->last_seen_ms = now;
            node->age_ms = 0;
            node->state = CLUSTER_NODE_ONLINE;
            node->missed_heartbeats = 0;
            node->health = 100;
            continue;
        }

        node->age_ms = now - node->last_seen_ms;
        node->state = cluster_manager_state_from_age(node->age_ms);

        if (node->state != CLUSTER_NODE_ONLINE)
            node->missed_heartbeats++;
        else
            node->missed_heartbeats = 0;

        node->health = calculate_health(node);

        if (old_state != node->state && event_count < max_events)
        {
            events[event_count].node_id = node->node_id;
            events[event_count].type =
                (node->state == CLUSTER_NODE_ONLINE)  ? EVENT_NODE_ONLINE :
                (node->state == CLUSTER_NODE_SUSPECT) ? EVENT_NODE_SUSPECT :
                                                       EVENT_NODE_OFFLINE;
            event_count++;
        }
    }

    return event_count;
}

static void cluster_manager_publish_events(cluster_event_t *events, int event_count)
{
    for (int i = 0; i < event_count; i++)
        cluster_publish_event(&events[i]);
}

static void print_snapshot_summary(const cluster_node_t *snapshot)
{
    int online = 0;
    int suspect = 0;
    int offline = 0;
    uint32_t self_age = 0;
    uint8_t self_health = 0;

    for (int i = 0; i < MAX_NODES; i++)
    {
        if (snapshot[i].node_id == 0)
            continue;

        if (snapshot[i].state == CLUSTER_NODE_ONLINE)
            online++;
        else if (snapshot[i].state == CLUSTER_NODE_SUSPECT)
            suspect++;
        else
            offline++;

        if (snapshot[i].node_id == self_node_id)
        {
            self_age = snapshot[i].age_ms;
            self_health = snapshot[i].health;
        }
    }

    ESP_LOGI(TAG,
        "Resumo cluster: self=%" PRIu32 " online=%d suspect=%d offline=%d self_age=%" PRIu32 "ms self_health=%u%%",
        self_node_id,
        online,
        suspect,
        offline,
        self_age,
        self_health);
}

/* ============================================================
   MASTER (ANTI SPLIT-BRAIN)
============================================================ */

uint32_t cluster_get_master_node(void)
{
    cluster_node_t snapshot[MAX_NODES];

    portENTER_CRITICAL(&cluster_manager_lock);
    cluster_manager_copy_nodes_locked(snapshot);
    portEXIT_CRITICAL(&cluster_manager_lock);

    return cluster_manager_get_master_from_snapshot(snapshot);
}

/* ============================================================
   UPDATE FROM DISCOVERY
============================================================ */

void cluster_manager_update_node(uint32_t node_id, uint32_t ip)
{
    cluster_node_t *node;
    cluster_node_state_t old_state;
    bool known_before;
    bool log_new_node;
    bool publish_online_event = false;
    cluster_event_t evt = {0};

    portENTER_CRITICAL(&cluster_manager_lock);

    node = get_or_create_node(node_id);
    if (!node)
    {
        portEXIT_CRITICAL(&cluster_manager_lock);
        return;
    }

    old_state = node->state;
    known_before = (node->last_seen_ms != 0U);
    log_new_node = (old_state == CLUSTER_NODE_OFFLINE && !known_before);

    node->ip = ip;
    node->last_seen_ms = now_ms();
    node->age_ms = 0;

    if (node->state == CLUSTER_NODE_OFFLINE && known_before)
        node->recoveries++;

    node->missed_heartbeats = 0;
    node->state = CLUSTER_NODE_ONLINE;
    node->health = 100;

    if (node_id != self_node_id && old_state != CLUSTER_NODE_ONLINE)
    {
        evt.node_id = node_id;
        evt.type = EVENT_NODE_ONLINE;
        publish_online_event = true;
    }

    portEXIT_CRITICAL(&cluster_manager_lock);

    if (log_new_node)
        ESP_LOGI(TAG, "Novo node: %" PRIu32, node_id);

    if (publish_online_event)
        cluster_publish_event(&evt);
}

void cluster_manager_remove_node(uint32_t node_id)
{
    cluster_node_t *node;
    bool removed = false;

    portENTER_CRITICAL(&cluster_manager_lock);

    node = find_node(node_id);
    if (node && node->node_id != 0U && node->node_id != self_node_id)
    {
        memset(node, 0, sizeof(*node));
        removed = true;
    }

    portEXIT_CRITICAL(&cluster_manager_lock);

    if (removed)
        ESP_LOGI(TAG, "Node removido do snapshot: %" PRIu32, node_id);
}

void cluster_manager_start(uint32_t self_id)
{
    cluster_node_t *self_node;

    if (cluster_manager_started)
        return;

    portENTER_CRITICAL(&cluster_manager_lock);

    memset(nodes, 0, sizeof(nodes));
    self_node_id = self_id;

    self_node = get_or_create_node(self_node_id);
    if (self_node)
    {
        self_node->last_seen_ms = now_ms();
        self_node->age_ms = 0;
        self_node->state = CLUSTER_NODE_ONLINE;
        self_node->missed_heartbeats = 0;
        self_node->health = 100;
    }

    cluster_manager_started = true;
    cluster_manager_last_run_ms = 0;

    portEXIT_CRITICAL(&cluster_manager_lock);

    ESP_LOGI(TAG, "Cluster Manager iniciado (self=%" PRIu32 ")", self_node_id);
}

void cluster_manager_process(void)
{
    int event_count;
    uint32_t now;

    if (!cluster_manager_started)
        return;

    now = now_ms();

    portENTER_CRITICAL(&cluster_manager_lock);

    if (cluster_manager_last_run_ms != 0U &&
        (uint32_t)(now - cluster_manager_last_run_ms) < CLUSTER_MANAGER_TICK_MS)
    {
        portEXIT_CRITICAL(&cluster_manager_lock);
        return;
    }

    cluster_manager_last_run_ms = now;
    event_count = cluster_manager_refresh_nodes_locked(
        cluster_manager_events_buf,
        CLUSTER_MANAGER_MAX_PENDING_EVENTS);
    cluster_manager_copy_nodes_locked(cluster_manager_snapshot_buf);

    portEXIT_CRITICAL(&cluster_manager_lock);

    cluster_manager_publish_events(cluster_manager_events_buf, event_count);
    print_snapshot_summary(cluster_manager_snapshot_buf);
}

cluster_metrics_t cluster_get_metrics(void)
{
    cluster_metrics_t metrics = {0};
    cluster_node_t snapshot[MAX_NODES];
    uint32_t health_sum = 0;
    bool active;
    uint32_t local_self_node;

    portENTER_CRITICAL(&cluster_manager_lock);
    cluster_manager_copy_nodes_locked(snapshot);
    active = cluster_manager_started;
    local_self_node = self_node_id;
    portEXIT_CRITICAL(&cluster_manager_lock);

    metrics.active = active ? 1U : 0U;
    metrics.self_node = local_self_node;
    metrics.master_node = cluster_manager_get_master_from_snapshot(snapshot);

    for (int i = 0; i < MAX_NODES; i++)
    {
        if (snapshot[i].node_id == 0U)
            continue;

        metrics.total_nodes++;
        health_sum += snapshot[i].health;

        if (snapshot[i].state == CLUSTER_NODE_ONLINE)
            metrics.online++;
        else if (snapshot[i].state == CLUSTER_NODE_SUSPECT)
            metrics.suspect++;
        else
            metrics.offline++;
    }

    if (metrics.total_nodes > 0U)
        metrics.avg_health = health_sum / metrics.total_nodes;

    return metrics;
}

int cluster_manager_export_nodes(cluster_node_t *out_nodes, int max_nodes)
{
    cluster_node_t snapshot[MAX_NODES];
    int copied = 0;

    if (!out_nodes || max_nodes <= 0)
        return 0;

    portENTER_CRITICAL(&cluster_manager_lock);
    cluster_manager_copy_nodes_locked(snapshot);
    portEXIT_CRITICAL(&cluster_manager_lock);

    for (int i = 0; i < MAX_NODES && copied < max_nodes; i++)
    {
        if (snapshot[i].node_id == 0U)
            continue;

        out_nodes[copied++] = snapshot[i];
    }

    return copied;
}

#include "node_registry.h"

#include "cluster_manager.h"
#include "cluster_transport.h"

#include "esp_log.h"
#include "esp_timer.h"
#include "nvs.h"

#include "freertos/FreeRTOS.h"
#include "freertos/portmacro.h"

#include <ctype.h>
#include <inttypes.h>
#include <string.h>

#define TAG "NODE_REG"
#define NODE_REGISTRY_NAMESPACE "node_registry"
#define NODE_REGISTRY_KEY       "nodes_v1"
#define NODE_REGISTRY_MAGIC     0x4E524547U
#define NODE_REGISTRY_VERSION   2U
#define NODE_REGISTRY_VERSION_V1 1U
#define NODE_REGISTRY_SYNC_MS   1000U

typedef struct
{
    uint32_t magic;
    uint16_t version;
    uint16_t count;
    node_registry_entry_t entries[NODE_REGISTRY_MAX_NODES];
    uint32_t crc;
} node_registry_blob_t;

typedef struct
{
    uint32_t node_id;
    uint32_t last_ip_addr;
    uint32_t age_ms;
    uint8_t health;
    uint8_t cluster_state;
    uint8_t registry_state;
    uint8_t reserved;
    char profile[NODE_REGISTRY_PROFILE_LEN];
    char template_name[NODE_REGISTRY_TEMPLATE_LEN];
} node_registry_entry_v1_t;

typedef struct
{
    uint32_t magic;
    uint16_t version;
    uint16_t count;
    node_registry_entry_v1_t entries[NODE_REGISTRY_MAX_NODES];
    uint32_t crc;
} node_registry_blob_v1_t;

static node_registry_entry_t entries[NODE_REGISTRY_MAX_NODES];
static bool node_registry_initialized = false;
static uint32_t last_sync_ms = 0;
static portMUX_TYPE node_registry_lock = portMUX_INITIALIZER_UNLOCKED;

static uint32_t node_registry_now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000ULL);
}

static uint32_t node_registry_crc32(const uint8_t *data, size_t len)
{
    uint32_t crc = 2166136261u;

    for (size_t i = 0; i < len; i++)
    {
        crc ^= data[i];
        crc *= 16777619u;
    }

    return crc;
}

static void node_registry_copy_text(char *dst, size_t dst_size, const char *src)
{
    if (!dst || dst_size == 0U)
        return;

    if (!src)
        src = "";

    strncpy(dst, src, dst_size - 1U);
    dst[dst_size - 1U] = '\0';
}


static uint8_t node_registry_transport_from_cluster(uint8_t transport)
{
    switch ((cluster_transport_type_t)transport)
    {
        case CLUSTER_TRANSPORT_WIFI_UDP:
            return NODE_REGISTRY_TRANSPORT_WIFI_UDP;
        case CLUSTER_TRANSPORT_ETHERNET_UDP:
            return NODE_REGISTRY_TRANSPORT_ETHERNET_UDP;
        case CLUSTER_TRANSPORT_RS485:
            return NODE_REGISTRY_TRANSPORT_RS485_CLUSTER;
        case CLUSTER_TRANSPORT_NONE:
        default:
            return NODE_REGISTRY_TRANSPORT_NONE;
    }
}

static uint8_t node_registry_compute_offline_reason(const node_registry_entry_t *entry)
{
    if (!entry)
        return NODE_REGISTRY_OFFLINE_NONE;

    switch ((cluster_node_state_t)entry->cluster_state)
    {
        case CLUSTER_NODE_ONLINE:
            return NODE_REGISTRY_OFFLINE_NONE;
        case CLUSTER_NODE_SUSPECT:
            return NODE_REGISTRY_OFFLINE_LINK_DEGRADED;
        case CLUSTER_NODE_OFFLINE:
        default:
            if (entry->last_transport == NODE_REGISTRY_TRANSPORT_RS485_CLUSTER)
                return NODE_REGISTRY_OFFLINE_HEARTBEAT_TIMEOUT;

            return entry->last_ip_addr == 0U
                ? NODE_REGISTRY_OFFLINE_MISSING_ADDRESS
                : NODE_REGISTRY_OFFLINE_HEARTBEAT_TIMEOUT;
    }
}

static uint8_t node_registry_default_recovery_caps(void)
{
    return NODE_REGISTRY_RECOVERY_TRY_RECONNECT |
           NODE_REGISTRY_RECOVERY_REENABLE_WIFI |
           NODE_REGISTRY_RECOVERY_FORCE_MODE |
           NODE_REGISTRY_RECOVERY_IDENTIFY;
}

static bool node_registry_profile_is_supported(const char *profile)
{
    static const char *const supported_profiles[] =
    {
        "gateway",
        "field-node",
        "relay-node",
        "sensor-node",
        "local-io-node",
    };

    if (!profile || profile[0] == '\0')
        return false;

    for (size_t i = 0; i < (sizeof(supported_profiles) / sizeof(supported_profiles[0])); i++)
    {
        if (strcmp(profile, supported_profiles[i]) == 0)
            return true;
    }

    return false;
}

static bool node_registry_template_is_valid(const char *template_name)
{
    const unsigned char *p = (const unsigned char *)(template_name ? template_name : "");

    while (*p != '\0')
    {
        if (!(isalnum(*p) || *p == '-' || *p == '_' || *p == '.'))
            return false;

        p++;
    }

    return true;
}

static node_registry_entry_t *node_registry_find_locked(uint32_t node_id);

static void node_registry_restore_entry(uint32_t node_id, const node_registry_entry_t *previous)
{
    node_registry_entry_t *entry;

    if (node_id == 0U || !previous)
        return;

    portENTER_CRITICAL(&node_registry_lock);
    entry = node_registry_find_locked(node_id);
    if (entry)
        *entry = *previous;
    portEXIT_CRITICAL(&node_registry_lock);
}

static bool node_registry_should_persist(const node_registry_entry_t *entry)
{
    if (!entry || entry->node_id == 0U)
        return false;

    return entry->registry_state != NODE_REGISTRY_STATE_DISCOVERED ||
           entry->profile[0] != '\0' ||
           entry->template_name[0] != '\0';
}

static node_registry_entry_t *node_registry_find_locked(uint32_t node_id)
{
    for (int i = 0; i < NODE_REGISTRY_MAX_NODES; i++)
    {
        if (entries[i].node_id == node_id)
            return &entries[i];
    }

    return NULL;
}

static node_registry_entry_t *node_registry_allocate_locked(void)
{
    for (int i = 0; i < NODE_REGISTRY_MAX_NODES; i++)
    {
        if (entries[i].node_id == 0U)
            return &entries[i];
    }

    return NULL;
}

static void node_registry_reset_runtime(node_registry_entry_t *entry)
{
    if (!entry || entry->node_id == 0U)
        return;

    entry->age_ms = 0U;
    entry->last_seen_ms = 0U;
    entry->health = 0U;
    entry->cluster_state = (uint8_t)CLUSTER_NODE_OFFLINE;
    entry->offline_reason = node_registry_compute_offline_reason(entry);
    entry->recovery_capabilities = node_registry_default_recovery_caps();
}

static bool node_registry_seed_entry_from_cluster(uint32_t node_id)
{
    cluster_node_t snapshot[MAX_NODES];
    cluster_node_t target = {0};
    node_registry_entry_t *entry;
    bool found = false;
    int count;

    if (node_id == 0U)
        return false;

    count = cluster_manager_export_nodes(snapshot, MAX_NODES);

    for (int i = 0; i < count; i++)
    {
        if (snapshot[i].node_id == node_id)
        {
            target = snapshot[i];
            found = true;
            break;
        }
    }

    if (!found)
        return false;

    portENTER_CRITICAL(&node_registry_lock);

    entry = node_registry_find_locked(node_id);
    if (!entry)
    {
        entry = node_registry_allocate_locked();

        if (entry)
        {
            memset(entry, 0, sizeof(*entry));
            entry->node_id = node_id;
            entry->registry_state = NODE_REGISTRY_STATE_DISCOVERED;
        }
    }

    if (entry)
    {
        entry->last_ip_addr = target.ip;
        entry->age_ms = target.age_ms;
        entry->last_seen_ms = target.last_seen_ms;
        entry->health = target.health;
        entry->cluster_state = (uint8_t)target.state;
        if (entry->last_transport == NODE_REGISTRY_TRANSPORT_NONE)
            entry->last_transport = NODE_REGISTRY_TRANSPORT_NONE;
        entry->offline_reason = node_registry_compute_offline_reason(entry);
        entry->recovery_capabilities = node_registry_default_recovery_caps();
        found = true;
    }
    else
    {
        found = false;
    }

    portEXIT_CRITICAL(&node_registry_lock);
    return found;
}

static void node_registry_build_blob_locked(node_registry_blob_t *blob)
{
    if (!blob)
        return;

    memset(blob, 0, sizeof(*blob));
    blob->magic = NODE_REGISTRY_MAGIC;
    blob->version = NODE_REGISTRY_VERSION;

    for (int i = 0; i < NODE_REGISTRY_MAX_NODES; i++)
    {
        node_registry_entry_t persisted;

        if (!node_registry_should_persist(&entries[i]) ||
            blob->count >= NODE_REGISTRY_MAX_NODES)
        {
            continue;
        }

        persisted = entries[i];
        node_registry_reset_runtime(&persisted);
        blob->entries[blob->count++] = persisted;
    }

    blob->crc = node_registry_crc32((const uint8_t *)blob->entries,
                                    blob->count * sizeof(node_registry_entry_t));
}

static bool node_registry_save_blob(const node_registry_blob_t *blob)
{
    nvs_handle_t nvs;

    if (!blob)
        return false;

    if (nvs_open(NODE_REGISTRY_NAMESPACE, NVS_READWRITE, &nvs) != ESP_OK)
    {
        ESP_LOGE(TAG, "Falha ao abrir NVS do registry");
        return false;
    }

    if (nvs_set_blob(nvs, NODE_REGISTRY_KEY, blob, sizeof(*blob)) != ESP_OK)
    {
        nvs_close(nvs);
        ESP_LOGE(TAG, "Falha ao gravar node registry");
        return false;
    }

    if (nvs_commit(nvs) != ESP_OK)
    {
        nvs_close(nvs);
        ESP_LOGE(TAG, "Falha ao confirmar node registry");
        return false;
    }

    nvs_close(nvs);
    return true;
}

static bool node_registry_load_blob(node_registry_blob_t *blob)
{
    nvs_handle_t nvs;
    size_t len = 0U;

    if (!blob)
        return false;

    if (nvs_open(NODE_REGISTRY_NAMESPACE, NVS_READONLY, &nvs) != ESP_OK)
        return false;

    if (nvs_get_blob(nvs, NODE_REGISTRY_KEY, NULL, &len) != ESP_OK)
    {
        nvs_close(nvs);
        return false;
    }

    if (len == sizeof(*blob))
    {
        size_t read_len = sizeof(*blob);

        if (nvs_get_blob(nvs, NODE_REGISTRY_KEY, blob, &read_len) != ESP_OK)
        {
            nvs_close(nvs);
            return false;
        }

        nvs_close(nvs);

        if (blob->magic != NODE_REGISTRY_MAGIC || blob->version != NODE_REGISTRY_VERSION)
            return false;

        if (blob->count > NODE_REGISTRY_MAX_NODES)
            return false;

        return blob->crc == node_registry_crc32((const uint8_t *)blob->entries,
                                                blob->count * sizeof(node_registry_entry_t));
    }

    if (len == sizeof(node_registry_blob_v1_t))
    {
        node_registry_blob_v1_t legacy = {0};
        size_t read_len = sizeof(legacy);

        if (nvs_get_blob(nvs, NODE_REGISTRY_KEY, &legacy, &read_len) != ESP_OK)
        {
            nvs_close(nvs);
            return false;
        }

        nvs_close(nvs);

        if (legacy.magic != NODE_REGISTRY_MAGIC || legacy.version != NODE_REGISTRY_VERSION_V1)
            return false;

        if (legacy.count > NODE_REGISTRY_MAX_NODES)
            return false;

        if (legacy.crc != node_registry_crc32((const uint8_t *)legacy.entries,
                                              legacy.count * sizeof(node_registry_entry_v1_t)))
        {
            return false;
        }

        memset(blob, 0, sizeof(*blob));
        blob->magic = NODE_REGISTRY_MAGIC;
        blob->version = NODE_REGISTRY_VERSION;
        blob->count = legacy.count;

        for (uint16_t i = 0; i < legacy.count && i < NODE_REGISTRY_MAX_NODES; i++)
        {
            blob->entries[i].node_id = legacy.entries[i].node_id;
            blob->entries[i].last_ip_addr = legacy.entries[i].last_ip_addr;
            blob->entries[i].age_ms = legacy.entries[i].age_ms;
            blob->entries[i].last_seen_ms = 0U;
            blob->entries[i].health = legacy.entries[i].health;
            blob->entries[i].cluster_state = legacy.entries[i].cluster_state;
            blob->entries[i].registry_state = legacy.entries[i].registry_state;
            blob->entries[i].last_transport = NODE_REGISTRY_TRANSPORT_NONE;
            blob->entries[i].offline_reason = NODE_REGISTRY_OFFLINE_NONE;
            blob->entries[i].recovery_capabilities = node_registry_default_recovery_caps();
            node_registry_copy_text(blob->entries[i].profile,
                                    sizeof(blob->entries[i].profile),
                                    legacy.entries[i].profile);
            node_registry_copy_text(blob->entries[i].template_name,
                                    sizeof(blob->entries[i].template_name),
                                    legacy.entries[i].template_name);
        }

        blob->crc = node_registry_crc32((const uint8_t *)blob->entries,
                                        blob->count * sizeof(node_registry_entry_t));
        return true;
    }

    nvs_close(nvs);
    return false;
}

void node_registry_init(void)
{
    node_registry_blob_t blob;

    portENTER_CRITICAL(&node_registry_lock);
    memset(entries, 0, sizeof(entries));
    last_sync_ms = 0U;
    portEXIT_CRITICAL(&node_registry_lock);

    if (node_registry_load_blob(&blob))
    {
        portENTER_CRITICAL(&node_registry_lock);

        for (uint16_t i = 0; i < blob.count && i < NODE_REGISTRY_MAX_NODES; i++)
        {
            entries[i] = blob.entries[i];
            node_registry_reset_runtime(&entries[i]);
        }

        portEXIT_CRITICAL(&node_registry_lock);

        ESP_LOGI(TAG, "Node registry restaurado (%u node(s))", blob.count);
    }

    node_registry_initialized = true;
}

void node_registry_process(void)
{
    cluster_node_t snapshot[MAX_NODES];
    cluster_metrics_t metrics;
    uint32_t now_ms;
    int count;
    uint32_t new_nodes[NODE_REGISTRY_MAX_NODES];
    int new_count = 0;
    bool overflow = false;

    if (!node_registry_initialized)
        return;

    now_ms = node_registry_now_ms();

    portENTER_CRITICAL(&node_registry_lock);
    if ((now_ms - last_sync_ms) < NODE_REGISTRY_SYNC_MS)
    {
        portEXIT_CRITICAL(&node_registry_lock);
        return;
    }
    last_sync_ms = now_ms;
    portEXIT_CRITICAL(&node_registry_lock);

    count = cluster_manager_export_nodes(snapshot, MAX_NODES);
    metrics = cluster_get_metrics();

    portENTER_CRITICAL(&node_registry_lock);

    for (int i = 0; i < NODE_REGISTRY_MAX_NODES; i++)
        node_registry_reset_runtime(&entries[i]);

    for (int i = 0; i < count; i++)
    {
        node_registry_entry_t *entry;
        bool is_new = false;

        if (snapshot[i].node_id == 0U || snapshot[i].node_id == metrics.self_node)
            continue;

        entry = node_registry_find_locked(snapshot[i].node_id);

        if (!entry)
        {
            entry = node_registry_allocate_locked();

            if (!entry)
            {
                overflow = true;
                continue;
            }

            memset(entry, 0, sizeof(*entry));
            entry->node_id = snapshot[i].node_id;
            entry->registry_state = NODE_REGISTRY_STATE_DISCOVERED;
            is_new = true;
        }

        entry->last_ip_addr = snapshot[i].ip;
        entry->age_ms = snapshot[i].age_ms;
        entry->last_seen_ms = snapshot[i].last_seen_ms;
        entry->health = snapshot[i].health;
        entry->cluster_state = (uint8_t)snapshot[i].state;
        if (entry->last_transport == NODE_REGISTRY_TRANSPORT_NONE)
            entry->last_transport = NODE_REGISTRY_TRANSPORT_NONE;
        entry->offline_reason = node_registry_compute_offline_reason(entry);
        entry->recovery_capabilities = node_registry_default_recovery_caps();

        if (is_new && new_count < NODE_REGISTRY_MAX_NODES)
            new_nodes[new_count++] = snapshot[i].node_id;
    }

    portEXIT_CRITICAL(&node_registry_lock);

    if (overflow)
        ESP_LOGW(TAG, "Node registry cheio, novos peers nao serao rastreados");

    for (int i = 0; i < new_count; i++)
        ESP_LOGI(TAG, "Node descoberto para onboarding: %" PRIu32, new_nodes[i]);
}

int node_registry_export(node_registry_entry_t *out_entries, int max_entries)
{
    int copied = 0;

    if (!out_entries || max_entries <= 0)
        return 0;

    portENTER_CRITICAL(&node_registry_lock);

    for (int i = 0; i < NODE_REGISTRY_MAX_NODES && copied < max_entries; i++)
    {
        if (entries[i].node_id == 0U)
            continue;

        out_entries[copied++] = entries[i];
    }

    portEXIT_CRITICAL(&node_registry_lock);

    for (int i = 0; i < copied - 1; i++)
    {
        for (int j = i + 1; j < copied; j++)
        {
            if (out_entries[j].node_id < out_entries[i].node_id)
            {
                node_registry_entry_t tmp = out_entries[i];
                out_entries[i] = out_entries[j];
                out_entries[j] = tmp;
            }
        }
    }

    return copied;
}


bool node_registry_note_transport(uint32_t node_id, uint8_t transport, uint32_t source_ip)
{
    node_registry_entry_t *entry;
    bool updated = false;
    uint8_t mapped_transport;

    if (node_id == 0U)
        return false;

    mapped_transport = node_registry_transport_from_cluster(transport);

    portENTER_CRITICAL(&node_registry_lock);

    entry = node_registry_find_locked(node_id);
    if (!entry)
    {
        entry = node_registry_allocate_locked();
        if (entry)
        {
            memset(entry, 0, sizeof(*entry));
            entry->node_id = node_id;
            entry->registry_state = NODE_REGISTRY_STATE_DISCOVERED;
        }
    }

    if (entry)
    {
        if (source_ip != 0U)
            entry->last_ip_addr = source_ip;
        if (mapped_transport != NODE_REGISTRY_TRANSPORT_NONE)
            entry->last_transport = mapped_transport;
        entry->offline_reason = node_registry_compute_offline_reason(entry);
        updated = true;
    }

    portEXIT_CRITICAL(&node_registry_lock);
    return updated;
}

node_registry_state_t node_registry_get_state(uint32_t node_id)
{
    node_registry_state_t state = NODE_REGISTRY_STATE_DISCOVERED;

    if (node_id == 0U)
        return NODE_REGISTRY_STATE_DISCOVERED;

    portENTER_CRITICAL(&node_registry_lock);

    node_registry_entry_t *entry = node_registry_find_locked(node_id);
    if (entry)
        state = (node_registry_state_t)entry->registry_state;

    portEXIT_CRITICAL(&node_registry_lock);
    return state;
}

bool node_registry_is_known(uint32_t node_id)
{
    bool known = false;

    if (node_id == 0U)
        return false;

    portENTER_CRITICAL(&node_registry_lock);

    known = (node_registry_find_locked(node_id) != NULL);

    portEXIT_CRITICAL(&node_registry_lock);
    return known;
}

bool node_registry_is_adopted(uint32_t node_id)
{
    return node_registry_get_state(node_id) >= NODE_REGISTRY_STATE_ADOPTED;
}

bool node_registry_is_operational(uint32_t node_id)
{
    return node_registry_get_state(node_id) >= NODE_REGISTRY_STATE_ACTIVE;
}

bool node_registry_adopt(uint32_t node_id)
{
    bool found = false;
    node_registry_blob_t blob;
    node_registry_entry_t previous = {0};

    if (node_id == 0U)
        return false;

    portENTER_CRITICAL(&node_registry_lock);

    node_registry_entry_t *entry = node_registry_find_locked(node_id);
    if (!entry)
    {
        portEXIT_CRITICAL(&node_registry_lock);
        if (!node_registry_seed_entry_from_cluster(node_id))
            return false;
        portENTER_CRITICAL(&node_registry_lock);
        entry = node_registry_find_locked(node_id);
    }

    if (entry)
    {
        previous = *entry;

        if (entry->registry_state < NODE_REGISTRY_STATE_ADOPTED)
            entry->registry_state = NODE_REGISTRY_STATE_ADOPTED;

        node_registry_build_blob_locked(&blob);
        found = true;
    }

    portEXIT_CRITICAL(&node_registry_lock);

    if (found)
        ESP_LOGI(TAG, "Node adotado: %" PRIu32, node_id);

    if (!found)
        return false;

    if (node_registry_save_blob(&blob))
        return true;

    node_registry_restore_entry(node_id, &previous);
    ESP_LOGE(TAG, "Falha ao persistir adocao do node %" PRIu32, node_id);
    return false;
}

bool node_registry_configure(uint32_t node_id, const char *profile, const char *template_name)
{
    bool found = false;
    node_registry_blob_t blob;
    node_registry_entry_t previous = {0};
    node_registry_state_t preserved_state = NODE_REGISTRY_STATE_CONFIGURED;

    if (node_id == 0U ||
        !node_registry_profile_is_supported(profile) ||
        !node_registry_template_is_valid(template_name))
    {
        return false;
    }

    portENTER_CRITICAL(&node_registry_lock);

    node_registry_entry_t *entry = node_registry_find_locked(node_id);
    if (!entry)
    {
        portEXIT_CRITICAL(&node_registry_lock);
        if (!node_registry_seed_entry_from_cluster(node_id))
            return false;
        portENTER_CRITICAL(&node_registry_lock);
        entry = node_registry_find_locked(node_id);
    }

    if (entry)
    {
        previous = *entry;
        preserved_state = (node_registry_state_t)entry->registry_state;

        if (entry->registry_state < NODE_REGISTRY_STATE_ADOPTED)
            entry->registry_state = NODE_REGISTRY_STATE_ADOPTED;

        node_registry_copy_text(entry->profile, sizeof(entry->profile), profile);
        node_registry_copy_text(entry->template_name, sizeof(entry->template_name), template_name);
        entry->registry_state = (preserved_state >= NODE_REGISTRY_STATE_ACTIVE)
            ? NODE_REGISTRY_STATE_ACTIVE
            : NODE_REGISTRY_STATE_CONFIGURED;
        node_registry_build_blob_locked(&blob);
        found = true;
    }

    portEXIT_CRITICAL(&node_registry_lock);

    if (found)
        ESP_LOGI(TAG, "Node configurado: %" PRIu32 " profile=%s template=%s",
                 node_id,
                 profile,
                 template_name ? template_name : "");

    if (!found)
        return false;

    if (node_registry_save_blob(&blob))
        return true;

    node_registry_restore_entry(node_id, &previous);
    ESP_LOGE(TAG, "Falha ao persistir configuracao do node %" PRIu32, node_id);
    return false;
}

bool node_registry_activate(uint32_t node_id)
{
    bool found = false;
    node_registry_blob_t blob;
    node_registry_entry_t previous = {0};

    if (node_id == 0U)
        return false;

    portENTER_CRITICAL(&node_registry_lock);

    node_registry_entry_t *entry = node_registry_find_locked(node_id);
    if (!entry)
    {
        portEXIT_CRITICAL(&node_registry_lock);
        if (!node_registry_seed_entry_from_cluster(node_id))
            return false;
        portENTER_CRITICAL(&node_registry_lock);
        entry = node_registry_find_locked(node_id);
    }

    if (entry && entry->registry_state >= NODE_REGISTRY_STATE_ADOPTED)
    {
        previous = *entry;
        entry->registry_state = NODE_REGISTRY_STATE_ACTIVE;
        node_registry_build_blob_locked(&blob);
        found = true;
    }

    portEXIT_CRITICAL(&node_registry_lock);

    if (found)
        ESP_LOGI(TAG, "Node ativado: %" PRIu32, node_id);

    if (!found)
        return false;

    if (node_registry_save_blob(&blob))
        return true;

    node_registry_restore_entry(node_id, &previous);
    ESP_LOGE(TAG, "Falha ao persistir ativacao do node %" PRIu32, node_id);
    return false;
}


bool node_registry_revoke(uint32_t node_id)
{
    bool found = false;
    node_registry_blob_t blob;
    node_registry_entry_t previous = {0};

    if (node_id == 0U)
        return false;

    portENTER_CRITICAL(&node_registry_lock);

    node_registry_entry_t *entry = node_registry_find_locked(node_id);
    if (entry)
    {
        previous = *entry;
        entry->registry_state = NODE_REGISTRY_STATE_DISCOVERED;
        entry->profile[0] = '\0';
        entry->template_name[0] = '\0';
        node_registry_build_blob_locked(&blob);
        found = true;
    }

    portEXIT_CRITICAL(&node_registry_lock);

    if (found)
        ESP_LOGI(TAG, "Node revogado: %" PRIu32, node_id);

    if (!found)
        return false;

    if (node_registry_save_blob(&blob))
        return true;

    node_registry_restore_entry(node_id, &previous);
    ESP_LOGE(TAG, "Falha ao persistir revogacao do node %" PRIu32, node_id);
    return false;
}

const char *node_registry_state_name(node_registry_state_t state)
{
    switch (state)
    {
        case NODE_REGISTRY_STATE_ADOPTED:
            return "adopted";
        case NODE_REGISTRY_STATE_CONFIGURED:
            return "configured";
        case NODE_REGISTRY_STATE_ACTIVE:
            return "active";
        case NODE_REGISTRY_STATE_DISCOVERED:
        default:
            return "discovered";
    }
}

const char *node_registry_cluster_state_name(uint8_t cluster_state)
{
    switch ((cluster_node_state_t)cluster_state)
    {
        case CLUSTER_NODE_ONLINE:
            return "online";
        case CLUSTER_NODE_SUSPECT:
            return "suspect";
        case CLUSTER_NODE_OFFLINE:
        default:
            return "offline";
    }
}

const char *node_registry_transport_name(uint8_t transport)
{
    switch ((node_registry_transport_t)transport)
    {
        case NODE_REGISTRY_TRANSPORT_WIFI_UDP:
            return "wifi-udp";
        case NODE_REGISTRY_TRANSPORT_ETHERNET_UDP:
            return "ethernet-udp";
        case NODE_REGISTRY_TRANSPORT_RS485_CLUSTER:
            return "rs485-cluster";
        case NODE_REGISTRY_TRANSPORT_NONE:
        default:
            return "none";
    }
}

const char *node_registry_offline_reason_name(uint8_t reason)
{
    switch ((node_registry_offline_reason_t)reason)
    {
        case NODE_REGISTRY_OFFLINE_HEARTBEAT_TIMEOUT:
            return "heartbeat-timeout";
        case NODE_REGISTRY_OFFLINE_MISSING_ADDRESS:
            return "missing-address";
        case NODE_REGISTRY_OFFLINE_LINK_DEGRADED:
            return "link-degraded";
        case NODE_REGISTRY_OFFLINE_NONE:
        default:
            return "none";
    }
}

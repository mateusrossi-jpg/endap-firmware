#include "cluster_io.h"
#include "cluster_manager.h"
#include "cluster_transport.h"
#include "node_registry.h"
#include "protocol.h"

#include <stdint.h>
#include <inttypes.h>
#include <string.h>

#include "esp_log.h"

#include "freertos/FreeRTOS.h"
#include "freertos/portmacro.h"

#define TAG "CLUSTER_IO"

static cluster_io_entry_t io_table[MAX_IO];
static uint32_t self_node = 0;
static portMUX_TYPE cluster_io_lock = portMUX_INITIALIZER_UNLOCKED;

/* ============================================================
   FORWARD DECLARATIONS
============================================================ */

static void cluster_io_handle_update(const protocol_cluster_io_update_t *msg);
static int cluster_io_node_is_operational(uint32_t node_id);

/* ============================================================
   CALLBACK DO TRANSPORTE
============================================================ */

static void cluster_io_handle_transport_frame(const uint8_t *data, uint16_t len)
{
    protocol_process_frame(data, len);
}

static int cluster_io_node_is_operational(uint32_t node_id)
{
    if (node_id == 0U || node_id == self_node)
        return 1;

    return node_registry_is_operational(node_id) ? 1 : 0;
}

/* ============================================================
   INTERNAL HELPERS
============================================================ */

/* usar somente com lock já adquirido */
static cluster_io_entry_t *cluster_io_find_locked(uint32_t io_id)
{
    for (int i = 0; i < MAX_IO; i++)
    {
        if (!io_table[i].valid)
            continue;

        if (io_table[i].io_id == io_id)
            return &io_table[i];
    }

    return NULL;
}

/* usar somente com lock já adquirido */
static cluster_io_entry_t *cluster_io_allocate_locked(uint32_t io_id)
{
    cluster_io_entry_t *entry = cluster_io_find_locked(io_id);

    if (entry)
        return entry;

    for (int i = 0; i < MAX_IO; i++)
    {
        if (io_table[i].valid)
            continue;

        io_table[i].io_id = io_id;
        io_table[i].owner = self_node;
        io_table[i].original_owner = self_node;
        io_table[i].valid = 1;
        return &io_table[i];
    }

    return NULL;
}

static int cluster_io_copy_entry_by_index(int index, cluster_io_entry_t *out)
{
    int valid = 0;

    if (!out || index < 0 || index >= MAX_IO)
        return 0;

    portENTER_CRITICAL(&cluster_io_lock);

    if (io_table[index].valid)
    {
        *out = io_table[index];
        valid = 1;
    }

    portEXIT_CRITICAL(&cluster_io_lock);

    return valid;
}

static int cluster_io_copy_entry_by_id(uint32_t io_id, cluster_io_entry_t *out)
{
    int found = 0;

    if (!out)
        return 0;

    portENTER_CRITICAL(&cluster_io_lock);

    cluster_io_entry_t *entry = cluster_io_find_locked(io_id);
    if (entry)
    {
        *out = *entry;
        found = 1;
    }

    portEXIT_CRITICAL(&cluster_io_lock);

    return found;
}

/* ============================================================
   INTERNAL: BROADCAST
============================================================ */

static void cluster_io_broadcast_update(uint32_t io_id, uint32_t owner, uint32_t original_owner)
{
    protocol_msg_t msg = {0};

    msg.type = PROTOCOL_MSG_CLUSTER_IO_UPDATE;
    msg.data.io_update.io_id = io_id;
    msg.data.io_update.new_owner = owner;
    msg.data.io_update.original_owner = original_owner;

    cluster_transport_broadcast_frame((const uint8_t *)&msg, sizeof(msg));
}

/* ============================================================
   INIT
============================================================ */

void cluster_io_init(uint32_t node_id)
{
    portENTER_CRITICAL(&cluster_io_lock);
    self_node = node_id;
    memset(io_table, 0, sizeof(io_table));
    portEXIT_CRITICAL(&cluster_io_lock);

    protocol_register_io_update_callback(cluster_io_handle_update);
    cluster_transport_register_frame_callback(cluster_io_handle_transport_frame);

    ESP_LOGI(TAG, "Cluster IO iniciado (self=%" PRIu32 ")", self_node);
}

void cluster_io_register_local(uint32_t io_id)
{
    cluster_io_entry_t *io;

    if (io_id == 0U)
        return;

    portENTER_CRITICAL(&cluster_io_lock);

    io = cluster_io_allocate_locked(io_id);

    if (!io)
    {
        portEXIT_CRITICAL(&cluster_io_lock);
        ESP_LOGE(TAG, "Tabela IO cheia ao registrar IO %" PRIu32, io_id);
        return;
    }

    io->io_id = io_id;
    io->owner = self_node;
    io->original_owner = self_node;
    io->valid = 1;

    portEXIT_CRITICAL(&cluster_io_lock);

    /* propositalmente sem log por item */
}

void cluster_io_sync_all(void)
{
    int sync_count = 0;

    for (int i = 0; i < MAX_IO; i++)
    {
        cluster_io_entry_t entry;

        if (!cluster_io_copy_entry_by_index(i, &entry))
            continue;

        cluster_io_broadcast_update(entry.io_id, entry.owner, entry.original_owner);
        sync_count++;
    }

    ESP_LOGI(TAG, "SYNC broadcast de %d IO(s)", sync_count);
}

void cluster_io_set_owner(uint32_t io_id, uint32_t owner, uint32_t original_owner)
{
    cluster_io_entry_t *io;

    portENTER_CRITICAL(&cluster_io_lock);

    io = cluster_io_allocate_locked(io_id);

    if (!io)
    {
        portEXIT_CRITICAL(&cluster_io_lock);
        ESP_LOGE(TAG, "Tabela IO cheia ao setar owner do IO %" PRIu32, io_id);
        return;
    }

    io->io_id = io_id;
    io->owner = owner;
    io->original_owner = original_owner;
    io->valid = 1;

    portEXIT_CRITICAL(&cluster_io_lock);

    ESP_LOGI(TAG,
        "LOCAL IO %" PRIu32 " -> owner=%" PRIu32 " original=%" PRIu32,
        io_id,
        owner,
        original_owner);
}

/* ============================================================
   FAILOVER
============================================================ */

void cluster_io_handle_node_offline(uint32_t failed_node)
{
    uint32_t master = cluster_get_master_node();

    if (self_node != master)
    {
        ESP_LOGW(TAG,
            "SKIP takeover (nao sou master: %" PRIu32 ")",
            master);
        return;
    }

    ESP_LOGW(TAG, "FAILOVER MASTER: node %" PRIu32, failed_node);

    for (int i = 0; i < MAX_IO; i++)
    {
        cluster_io_entry_t changed;
        int did_change = 0;

        portENTER_CRITICAL(&cluster_io_lock);

        cluster_io_entry_t *io = &io_table[i];

        if (io->valid && io->owner == failed_node)
        {
            io->owner = self_node;
            changed = *io;
            did_change = 1;
        }

        portEXIT_CRITICAL(&cluster_io_lock);

        if (!did_change)
            continue;

        ESP_LOGW(TAG,
            "IO %" PRIu32 " assumido por %" PRIu32,
            changed.io_id,
            self_node);

        cluster_io_broadcast_update(changed.io_id, changed.owner, changed.original_owner);
    }
}

/* ============================================================
   FAILBACK
============================================================ */

void cluster_io_handle_node_online(uint32_t node_id)
{
    ESP_LOGI(TAG, "FAILBACK: node %" PRIu32, node_id);

    for (int i = 0; i < MAX_IO; i++)
    {
        cluster_io_entry_t changed;
        int did_change = 0;

        portENTER_CRITICAL(&cluster_io_lock);

        cluster_io_entry_t *io = &io_table[i];

        if (io->valid &&
            io->original_owner == node_id &&
            io->owner == self_node)
        {
            io->owner = node_id;
            changed = *io;
            did_change = 1;
        }

        portEXIT_CRITICAL(&cluster_io_lock);

        if (!did_change)
            continue;

        ESP_LOGI(TAG,
            "IO %" PRIu32 " devolvido para %" PRIu32,
            changed.io_id,
            node_id);

        cluster_io_broadcast_update(changed.io_id, changed.owner, changed.original_owner);
    }

    cluster_io_sync_all();
}

/* ============================================================
   UPDATE RECEBIDO
============================================================ */

static void cluster_io_handle_update(const protocol_cluster_io_update_t *msg)
{
    cluster_io_entry_t *io;
    uint32_t synced_io_id;
    uint32_t synced_owner;
    uint32_t synced_original_owner;

    if (!msg)
        return;

    if (!cluster_io_node_is_operational(msg->new_owner) ||
        !cluster_io_node_is_operational(msg->original_owner))
    {
        ESP_LOGW(TAG,
                 "SYNC IO %u ignorado: owner=%" PRIu32 " original=%" PRIu32 " ainda nao admitido(s)",
                 msg->io_id,
                 msg->new_owner,
                 msg->original_owner);
        return;
    }

    portENTER_CRITICAL(&cluster_io_lock);

    io = cluster_io_allocate_locked(msg->io_id);

    if (!io)
    {
        portEXIT_CRITICAL(&cluster_io_lock);
        ESP_LOGE(TAG, "Tabela IO cheia ao sincronizar IO %u", msg->io_id);
        return;
    }

    if (io->valid &&
        io->original_owner == self_node &&
        msg->original_owner != self_node)
    {
        portEXIT_CRITICAL(&cluster_io_lock);
        ESP_LOGW(TAG,
                 "SYNC IO %u ignorado: conflito com IO local (remoto owner=%" PRIu32 " original=%" PRIu32 ")",
                 msg->io_id,
                 msg->new_owner,
                 msg->original_owner);
        return;
    }

    if (io->owner == msg->new_owner &&
        io->original_owner == msg->original_owner)
    {
        portEXIT_CRITICAL(&cluster_io_lock);
        return;
    }

    io->io_id = msg->io_id;
    io->owner = msg->new_owner;
    io->original_owner = msg->original_owner;
    io->valid = 1;

    synced_io_id = io->io_id;
    synced_owner = io->owner;
    synced_original_owner = io->original_owner;

    portEXIT_CRITICAL(&cluster_io_lock);

    ESP_LOGI(TAG,
        "SYNC IO %" PRIu32 " -> owner=%" PRIu32 " original=%" PRIu32,
        synced_io_id,
        synced_owner,
        synced_original_owner);
}

/* ============================================================
   QUERY
============================================================ */

int cluster_io_is_local(uint32_t io_id)
{
    cluster_io_entry_t io;

    if (self_node == 0U)
        return 1;

    if (!cluster_io_copy_entry_by_id(io_id, &io))
        return 1;

    return (io.owner == self_node);
}

uint32_t cluster_io_get_owner(uint32_t io_id)
{
    cluster_io_entry_t io;

    if (!cluster_io_copy_entry_by_id(io_id, &io))
        return self_node;

    return io.owner;
}

uint32_t cluster_io_get_original_owner(uint32_t io_id)
{
    cluster_io_entry_t io;

    if (!cluster_io_copy_entry_by_id(io_id, &io))
        return self_node;

    return io.original_owner;
}

/* ============================================================
   DEBUG
============================================================ */

void cluster_io_dump(void)
{
    int count = 0;

    ESP_LOGI(TAG, "------ IO TABLE ------");

    for (int i = 0; i < MAX_IO; i++)
    {
        cluster_io_entry_t entry;

        if (!cluster_io_copy_entry_by_index(i, &entry))
            continue;

        ESP_LOGI(TAG,
            "IO %" PRIu32 " | owner=%" PRIu32 " | original=%" PRIu32,
            entry.io_id,
            entry.owner,
            entry.original_owner);

        count++;
    }

    ESP_LOGI(TAG, "------ TOTAL IO: %d ------", count);
}

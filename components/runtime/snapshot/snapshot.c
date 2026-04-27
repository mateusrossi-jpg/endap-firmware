#include "snapshot.h"
#include "state.h"

#include "esp_log.h"
#include "esp_timer.h"
#include "nvs.h"

#include <string.h>
#include <stddef.h>
#include <inttypes.h>

#define TAG "SNAPSHOT"
#define SNAPSHOT_DEBOUNCE_MS 5000
#define SNAPSHOT_NAMESPACE   "snapshot"
#define SNAPSHOT_KEY_STATE   "state_v1"
#define SNAPSHOT_MAGIC       0x534E4150U
#define SNAPSHOT_VERSION     1U

typedef struct
{
    uint32_t magic;
    uint16_t version;
    uint16_t count;
    uint32_t crc;
} snapshot_header_t;

static snapshot_storage_if_t *storage = NULL;

static bool snapshot_dirty = false;
static int64_t last_change = 0;
static uint32_t snapshot_crc = 0;

/* ============================================================
   CRC
============================================================ */

static uint32_t snapshot_crc32(const uint8_t *data, size_t len)
{
    uint32_t crc = 2166136261u;

    for (size_t i = 0; i < len; i++)
    {
        crc ^= data[i];
        crc *= 16777619u;
    }

    return crc;
}

/* ============================================================
   BACKEND
============================================================ */

static bool snapshot_backend_write(const void *buffer, uint32_t len)
{
    nvs_handle_t nvs;

    if (storage && storage->write)
        return storage->write(buffer, len);

    if (nvs_open(SNAPSHOT_NAMESPACE, NVS_READWRITE, &nvs) != ESP_OK)
    {
        ESP_LOGE(TAG, "Falha ao abrir NVS para snapshot");
        return false;
    }

    if (nvs_set_blob(nvs, SNAPSHOT_KEY_STATE, buffer, len) != ESP_OK)
    {
        nvs_close(nvs);
        ESP_LOGE(TAG, "Falha ao gravar snapshot");
        return false;
    }

    nvs_commit(nvs);
    nvs_close(nvs);
    return true;
}

static bool snapshot_backend_read(uint8_t *buffer, uint32_t max_len, uint32_t *out_len)
{
    nvs_handle_t nvs;
    size_t len = 0;

    if (!buffer || !out_len)
        return false;

    if (storage && storage->read)
    {
        if (!storage->read(buffer, max_len))
            return false;

        *out_len = max_len;
        return true;
    }

    if (nvs_open(SNAPSHOT_NAMESPACE, NVS_READONLY, &nvs) != ESP_OK)
        return false;

    if (nvs_get_blob(nvs, SNAPSHOT_KEY_STATE, NULL, &len) != ESP_OK || len == 0 || len > max_len)
    {
        nvs_close(nvs);
        return false;
    }

    if (nvs_get_blob(nvs, SNAPSHOT_KEY_STATE, buffer, &len) != ESP_OK)
    {
        nvs_close(nvs);
        return false;
    }

    nvs_close(nvs);
    *out_len = (uint32_t)len;
    return true;
}

/* ============================================================
   EXPORT
============================================================ */

uint16_t snapshot_export(uint8_t *buffer, uint16_t max_len)
{
    state_entry_t entries[STATE_MAX_ENTRIES];
    snapshot_header_t header = {0};
    uint16_t count;
    uint16_t payload_len;
    uint16_t total_len;

    if (!buffer)
        return 0;

    count = state_export_entries(entries, STATE_MAX_ENTRIES);
    payload_len = (uint16_t)(count * sizeof(state_entry_t));
    total_len = (uint16_t)(sizeof(header) + payload_len);

    if (total_len > max_len)
        return 0;

    header.magic = SNAPSHOT_MAGIC;
    header.version = SNAPSHOT_VERSION;
    header.count = count;

    memcpy(buffer, &header, sizeof(header));

    if (payload_len > 0)
        memcpy(buffer + sizeof(header), entries, payload_len);

    header.crc = snapshot_crc32(buffer + sizeof(header), payload_len);
    memcpy(buffer, &header, sizeof(header));
    snapshot_crc = header.crc;

    return total_len;
}

/* ============================================================
   RESTORE
============================================================ */

void snapshot_restore(const uint8_t *buffer, uint16_t len)
{
    const snapshot_header_t *header = (const snapshot_header_t *)buffer;
    const state_entry_t *entries;
    uint16_t payload_len;
    uint32_t crc;

    if (!buffer || len < sizeof(snapshot_header_t))
        return;

    if (header->magic != SNAPSHOT_MAGIC || header->version != SNAPSHOT_VERSION)
        return;

    payload_len = (uint16_t)(header->count * sizeof(state_entry_t));

    if ((uint16_t)(sizeof(snapshot_header_t) + payload_len) != len)
        return;

    crc = snapshot_crc32(buffer + sizeof(snapshot_header_t), payload_len);

    if (crc != header->crc)
    {
        ESP_LOGW(TAG, "Snapshot com CRC invalido");
        return;
    }

    entries = (const state_entry_t *)(buffer + sizeof(snapshot_header_t));

    for (uint16_t i = 0; i < header->count; i++)
    {
        state_set_int(entries[i].id, entries[i].value);
        state_clear_dirty(entries[i].id);
    }

    snapshot_crc = header->crc;
    snapshot_dirty = false;
    last_change = 0;

    ESP_LOGI(TAG, "Snapshot restaurado (%u estado(s))", header->count);
}

/* ============================================================
   SAVE
============================================================ */

static void snapshot_save(void)
{
    uint8_t buffer[sizeof(snapshot_header_t) + (STATE_MAX_ENTRIES * sizeof(state_entry_t))];
    state_entry_t entries[STATE_MAX_ENTRIES];
    uint16_t len = snapshot_export(buffer, sizeof(buffer));
    uint16_t count = state_export_entries(entries, STATE_MAX_ENTRIES);

    if (len == 0)
        return;

    if (!snapshot_backend_write(buffer, len))
        return;

    for (uint16_t i = 0; i < count; i++)
        state_clear_dirty(entries[i].id);

    snapshot_dirty = false;

    ESP_LOGI(TAG, "Snapshot salvo (%u estado(s), crc=%" PRIu32 ")", count, snapshot_crc);
}

/* ============================================================
   INIT
============================================================ */

void snapshot_init(void)
{
    uint8_t buffer[sizeof(snapshot_header_t) + (STATE_MAX_ENTRIES * sizeof(state_entry_t))];
    uint32_t len = 0;

    snapshot_dirty = false;
    snapshot_crc = 0;
    last_change = 0;

    if (snapshot_backend_read(buffer, sizeof(buffer), &len))
        snapshot_restore(buffer, (uint16_t)len);
}

/* ============================================================
   STORAGE
============================================================ */

void snapshot_set_storage(snapshot_storage_if_t *iface)
{
    storage = iface;
}

/* ============================================================
   MARK DIRTY
============================================================ */

void snapshot_mark_dirty(void)
{
    snapshot_dirty = true;
    last_change = esp_timer_get_time() / 1000;
}

/* ============================================================
   CRC / FORCE
============================================================ */

uint32_t snapshot_get_crc(void)
{
    return snapshot_crc;
}

void snapshot_force_save(void)
{
    snapshot_save();
}

/* ============================================================
   PROCESS
============================================================ */

void snapshot_process(void)
{
    int64_t now;

    if (!snapshot_dirty)
        return;

    now = esp_timer_get_time() / 1000;

    if ((now - last_change) >= SNAPSHOT_DEBOUNCE_MS)
        snapshot_save();
}

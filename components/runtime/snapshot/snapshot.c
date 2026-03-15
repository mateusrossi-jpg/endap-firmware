#include "snapshot.h"
#include "state.h"
#include "event_bus.h"

#include "esp_timer.h"
#include <string.h>

#define SNAPSHOT_DEBOUNCE_MS 5000

static snapshot_storage_if_t *storage = NULL;

static bool snapshot_dirty = false;
static int64_t last_change = 0;

static uint32_t snapshot_crc = 0;

/* ============================================================
INIT
============================================================ */

void snapshot_init(void)
{
    snapshot_dirty = false;
    snapshot_crc = 0;
}

/* ============================================================
SET STORAGE
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
CRC
============================================================ */

uint32_t snapshot_get_crc(void)
{
    return snapshot_crc;
}

/* ============================================================
EXPORT
============================================================ */

uint16_t snapshot_export(uint8_t *buffer, uint16_t max_len)
{
    if (!buffer)
        return 0;

    uint16_t pos = 0;

    for (uint16_t id = 0; id < STATE_MAX_ENTRIES; id++)
    {
        if (!state_is_dirty(id))
            continue;

        if (pos + 6 > max_len)
            break;

        int32_t value;

        if (!state_get_int(id, &value))
            continue;

        /* ============================================================
           EVENT GENERATION
        ============================================================ */

        endap_event_t ev = {
            .type = EVENT_INPUT_CHANGE,
            .source = id,
            .data = value
        };

        event_bus_publish(ev);

        /* ============================================================
           SNAPSHOT EXPORT
        ============================================================ */

        buffer[pos++] = id & 0xFF;
        buffer[pos++] = id >> 8;

        memcpy(&buffer[pos], &value, sizeof(int32_t));
        pos += sizeof(int32_t);

        state_clear_dirty(id);
    }

    /* ============================================================
       CRC CALCULATION
    ============================================================ */

    uint32_t crc = 0;

    for (uint16_t i = 0; i < pos; i++)
        crc ^= buffer[i];

    snapshot_crc = crc;

    return pos;
}

/* ============================================================
SAVE
============================================================ */

static void snapshot_save(void)
{
    uint8_t buffer[512];

    uint16_t len = snapshot_export(buffer, sizeof(buffer));

    if (len == 0)
        return;

    if (storage)
        storage->write(buffer, len);

    snapshot_dirty = false;
}

/* ============================================================
PROCESS
============================================================ */

void snapshot_process(void)
{
    if (!snapshot_dirty)
        return;

    int64_t now = esp_timer_get_time() / 1000;

    if ((now - last_change) >= SNAPSHOT_DEBOUNCE_MS)
        snapshot_save();
}

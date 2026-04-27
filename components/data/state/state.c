#include "state.h"

#include <string.h>

#include "esp_attr.h"

#include "event_bus.h"
#include "snapshot.h"

/* ============================================================
   OUTPUT HOOK (WEAK - OVERRIDE PELO IO_DRIVER)

   Hook chamado apenas na via normal de escrita do estado.
   Não é usado no import silencioso de snapshot.
============================================================ */

__attribute__((weak)) void state_output_changed(uint16_t id, int32_t value)
{
    /* default: não faz nada */
}

/* ============================================================
   STORAGE
============================================================ */

static int32_t int_values[STATE_MAX_ENTRIES] __attribute__((aligned(4)));
static bool dirty_flags[STATE_MAX_ENTRIES];

/* ============================================================
   INTERNAL
============================================================ */

static inline bool state_valid_id(uint16_t id)
{
    return (id < STATE_MAX_ENTRIES);
}

/* ============================================================
   INIT
============================================================ */

void state_init(void)
{
    memset(int_values, 0, sizeof(int_values));
    memset(dirty_flags, 0, sizeof(dirty_flags));
}

/* ============================================================
   BOOL API
============================================================ */

bool IRAM_ATTR state_set_bool(uint16_t id, bool value)
{
    return state_set_int(id, value ? 1 : 0);
}

bool IRAM_ATTR state_get_bool(uint16_t id, bool *out)
{
    int32_t value;

    if (out == NULL)
        return false;

    if (!state_get_int(id, &value))
        return false;

    *out = (value != 0);
    return true;
}

/* ============================================================
   SET INT (🔥 VIA NORMAL / OFICIAL DE ESCRITA)

   Contrato:
   - esta é a via normal de mutação de estado em runtime
   - usada por caminhos controlados do sistema
   - gera side effects quando o valor muda
============================================================ */

bool IRAM_ATTR state_set_int(uint16_t id, int32_t value)
{
    if (!state_valid_id(id))
        return false;

    if (int_values[id] != value)
    {
        int_values[id] = value;
        dirty_flags[id] = true;

        /* marca persistência pendente */
        snapshot_mark_dirty();

        endap_event_t ev = {
            .type = EVENT_STATE_CHANGE,
            .source = id,
            .data = value
        };

        /* notifica runtime/event-driven */
        event_bus_publish(ev);

        /* hook imediato para integração com IO */
        state_output_changed(id, value);
    }

    return true;
}

/* ============================================================
   GET INT
============================================================ */

bool IRAM_ATTR state_get_int(uint16_t id, int32_t *out)
{
    if (!state_valid_id(id) || out == NULL)
        return false;

    *out = int_values[id];
    return true;
}

/* ============================================================
   DIRTY ENGINE
============================================================ */

bool IRAM_ATTR state_is_dirty(uint16_t id)
{
    if (!state_valid_id(id))
        return false;

    return dirty_flags[id];
}

void IRAM_ATTR state_clear_dirty(uint16_t id)
{
    if (!state_valid_id(id))
        return;

    dirty_flags[id] = false;
}

/* ============================================================
   SNAPSHOT EXPORT
============================================================ */

uint16_t state_export_entries(state_entry_t *out, uint16_t max)
{
    if (out == NULL || max == 0)
        return 0;

    uint16_t count = 0;

    for (uint16_t id = 0; id < STATE_MAX_ENTRIES; id++)
    {
        int32_t val = int_values[id];

        if (val == 0 && !dirty_flags[id])
            continue;

        if (count >= max)
            break;

        out[count].id = id;
        out[count].value = val;
        count++;
    }

    return count;
}

uint16_t state_export_snapshot(int32_t *buffer, uint16_t max_entries)
{
    uint16_t count;

    if (buffer == NULL || max_entries == 0)
        return 0;

    count = (max_entries < STATE_MAX_ENTRIES) ? max_entries : STATE_MAX_ENTRIES;
    memcpy(buffer, int_values, sizeof(int32_t) * count);
    return count;
}

/* ============================================================
   SNAPSHOT IMPORT (🔥 VIA SILENCIOSA DE RESTORE)

   Contrato:
   - uso previsto: boot/recovery
   - não publica eventos
   - não aciona output hook
   - não marca snapshot dirty
   - limpa dirty_flags após restore
============================================================ */

void state_import_snapshot(const int32_t *buffer, uint16_t entries)
{
    uint16_t count;

    if (buffer == NULL)
        return;

    count = (entries < STATE_MAX_ENTRIES) ? entries : STATE_MAX_ENTRIES;

    memcpy(int_values, buffer, sizeof(int32_t) * count);
    memset(dirty_flags, 0, sizeof(dirty_flags));
}

void state_process_snapshot(void)
{
    /* reservado para processamento fora do caminho crítico */
}

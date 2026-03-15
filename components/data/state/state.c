#include "state.h"
#include <string.h>

//#include "pdo_pipeline.h"
//#include "state_journal.h"
#include "snapshot.h"

/* ============================================================
STORAGE
============================================================ */

static bool     bool_values[STATE_MAX_ENTRIES];
static int32_t  int_values[STATE_MAX_ENTRIES];
static bool     dirty_flags[STATE_MAX_ENTRIES];


/* ============================================================
INTERNAL GUARDS
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
    memset(bool_values, 0, sizeof(bool_values));
    memset(int_values, 0, sizeof(int_values));
    memset(dirty_flags, 0, sizeof(dirty_flags));
}


/* ============================================================
BOOL
============================================================ */

bool state_set_bool(uint16_t id, bool value)
{
    if(!state_valid_id(id))
        return false;

    if(bool_values[id] != value)
    {
        bool_values[id] = value;

        dirty_flags[id] = true;

        snapshot_mark_dirty();
    }

    return true;
}


bool state_get_bool(uint16_t id, bool *out)
{
    if(!state_valid_id(id) || out == NULL)
        return false;

    *out = bool_values[id];

    return true;
}


/* ============================================================
INT
============================================================ */

bool state_set_int(uint16_t id, int32_t value)
{
    if(!state_valid_id(id))
        return false;

    if(int_values[id] != value)
    {
        int_values[id] = value;

        dirty_flags[id] = true;

        snapshot_mark_dirty();

     //   pdo_pipeline_push(id,value);

    //    state_journal_append(id,value);
    }

    return true;
}


bool state_get_int(uint16_t id, int32_t *out)
{
    if(!state_valid_id(id) || out == NULL)
        return false;

    *out = int_values[id];

    return true;
}


/* ============================================================
DIRTY ENGINE
============================================================ */

bool state_is_dirty(uint16_t id)
{
    if(!state_valid_id(id))
        return false;

    return dirty_flags[id];
}


void state_clear_dirty(uint16_t id)
{
    if(!state_valid_id(id))
        return;

    dirty_flags[id] = false;
}


/* ============================================================
SNAPSHOT EXPORT (FULL)
============================================================ */

uint16_t state_export_snapshot(int32_t *buffer, uint16_t max_entries)
{
    if(buffer == NULL)
        return 0;

    uint16_t count = STATE_MAX_ENTRIES;

    if(max_entries < count)
        count = max_entries;

    if(count == 0)
        return 0;

    memcpy(
        buffer,
        int_values,
        count * sizeof(int32_t)
    );

    return count;
}


/* ============================================================
SNAPSHOT IMPORT
============================================================ */

void state_import_snapshot(const int32_t *buffer, uint16_t entries)
{
    if(buffer == NULL)
        return;

    if(entries > STATE_MAX_ENTRIES)
        entries = STATE_MAX_ENTRIES;

    for(uint16_t i = 0; i < entries; i++)
    {
        int32_t new_value = buffer[i];

        if(int_values[i] != new_value)
        {
            int_values[i] = new_value;

            dirty_flags[i] = true;

        //    pdo_pipeline_push(i,new_value);
        }
    }
}

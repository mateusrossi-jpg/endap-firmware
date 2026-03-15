#ifndef STATE_H
#define STATE_H

#include <stdint.h>
#include <stdbool.h>

/* ============================================================
   CONFIG
============================================================ */

#define STATE_MAX_ENTRIES 128


/* ============================================================
   INIT
============================================================ */

void state_init(void);


/* ============================================================
   BOOL
============================================================ */

bool state_set_bool(uint16_t id, bool value);
bool state_get_bool(uint16_t id, bool *out);


/* ============================================================
   INT
============================================================ */

bool state_set_int(uint16_t id, int32_t value);
bool state_get_int(uint16_t id, int32_t *out);


/* ============================================================
   DIRTY ENGINE
============================================================ */

bool state_is_dirty(uint16_t id);
void state_clear_dirty(uint16_t id);


/* ============================================================
   SNAPSHOT
============================================================ */

/* Exporta estado inteiro */
uint16_t state_export_snapshot(int32_t *buffer, uint16_t max_entries);

/* Aplica snapshot recebido */
void state_import_snapshot(const int32_t *buffer, uint16_t entries);


#endif
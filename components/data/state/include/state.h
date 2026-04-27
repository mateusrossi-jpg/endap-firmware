#ifndef STATE_H
#define STATE_H

#include <stdbool.h>
#include <stdint.h>

/* ============================================================
   MODULE CONTRACT

   - state_set_int()/state_set_bool() são a via oficial de escrita
     normal do estado em runtime.
   - Escritas normais podem gerar side effects:
       * dirty flag
       * snapshot_mark_dirty()
       * event_bus_publish()
       * state_output_changed()
   - Services / HTTP / rede não devem escrever no estado de forma
     arbitrária; devem preferir caminhos controlados (fila/comando/runtime).
   - state_import_snapshot() é um caminho excepcional de boot/recovery:
     restaura storage de forma silenciosa, sem eventos, sem output hook
     e sem deixar dirty pendente.
============================================================ */

/* ============================================================
   TYPES
============================================================ */

typedef struct
{
    uint16_t id;
    int32_t value;
} state_entry_t;

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
   SNAPSHOT (RAW - CLUSTER / BOOT / RECOVERY)

   state_import_snapshot():
   - uso previsto: boot/recovery/sincronização controlada
   - comportamento: silencioso
   - não publica eventos
   - não aciona output hook
   - limpa dirty flags após restore
============================================================ */

uint16_t state_export_snapshot(int32_t *buffer, uint16_t max_entries);
void state_import_snapshot(const int32_t *buffer, uint16_t entries);
void state_process_snapshot(void);

/* ============================================================
   SNAPSHOT (STRUCTURED - DASHBOARD)
============================================================ */

uint16_t state_export_entries(state_entry_t *out, uint16_t max);

#endif

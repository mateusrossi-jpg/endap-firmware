#ifndef V3_AUTOMATION_H
#define V3_AUTOMATION_H

#include <stdint.h>
#include <stdbool.h>
#include <esp_attr.h>
#include "v3_identity.h"

/* * Escrita de Estado Desejado (Instigado por Lógica ou API) */
bool v3_automation_write_desired(resource_id_t target_id, uint32_t desired_state);

/* * Atualização de Estado Reportado (Hot Path, chamado pelos Drivers físicos V2) */
bool v3_automation_update_reported(resource_id_t target_id, uint32_t reported_state);

#endif // V3_AUTOMATION_H

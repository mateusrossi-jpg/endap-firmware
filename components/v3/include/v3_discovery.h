#ifndef V3_DISCOVERY_H
#define V3_DISCOVERY_H

#include <stdint.h>
#include <stdbool.h>
#include "v3_identity.h"

/* Os 4 estados canônicos do Manifesto V3 */
typedef enum {
    NODE_STATE_UNKNOWN = 0, // Recém energizado (vácuo)
    NODE_STATE_PROBE,       // Sendo interrogado pelo Leaky Bucket
    NODE_STATE_ACTIVE,      // Identidade validada, Catálogo notificado
    NODE_STATE_LOST         // Watchdog detectou queda de pulso
} v3_node_state_t;

/* * A Ponte Invertida (Weak Hook)
 * O driver RS485 da V2 DEVE chamar esta função, mas NÃO deve importar este header.
 */
void v3_discovery_report_node(uint16_t physical_slot);

/* Injeção de resposta do Driver V2 para dentro da Máquina de Estados da V3 */
bool v3_discovery_inject_identity(uint16_t physical_slot, const resource_key_t *key);

/* Cadência administrativa (Deve ser amarrada a um Timer de 100ms no Cold Path) */
void v3_discovery_pulse_100ms(void);

#endif // V3_DISCOVERY_H

#include "include/v3_discovery.h"
#include "include/v3_events.h"
#include <string.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/portmacro.h>

/* Bit-vector de descoberta (32 * 32 = 1024 slots) */
static uint32_t s_probe_mask[32] = {0};
static v3_node_state_t s_node_states[1024] = {NODE_STATE_UNKNOWN};
static portMUX_TYPE s_discovery_spinlock = portMUX_INITIALIZER_UNLOCKED;

__attribute__((weak)) void v3_discovery_report_node(uint16_t physical_slot) {
    if (physical_slot >= 1024) return;
    
    portENTER_CRITICAL_ISR(&s_discovery_spinlock); // Protege contra ISR (Drivers Legados)
    s_probe_mask[physical_slot / 32] |= (1U << (physical_slot % 32));
    portEXIT_CRITICAL_ISR(&s_discovery_spinlock);
}

void v3_discovery_pulse_100ms(void) {
    for (int i = 0; i < 32; i++) {
        portENTER_CRITICAL(&s_discovery_spinlock); // Protege contra Task
        if (s_probe_mask[i] != 0) {
            // Encontra o primeiro bit setado
            int bit_idx = __builtin_ffs(s_probe_mask[i]) - 1;
            
            // Limpa o bit
            s_probe_mask[i] &= ~(1U << bit_idx);
            
            int physical_slot = i * 32 + bit_idx;
            
            // Muda o estado e dispara a investigação
            s_node_states[physical_slot] = NODE_STATE_PROBE;
            
            portEXIT_CRITICAL(&s_discovery_spinlock);
            return; // Apenas 1 nó por pulso, libera CPU para o Kernel
        }
        portEXIT_CRITICAL(&s_discovery_spinlock);
    }
}

bool v3_discovery_inject_identity(uint16_t physical_slot, const resource_key_t *key) {
    if (physical_slot >= 1024 || !key) return false;
    
    s_node_states[physical_slot] = NODE_STATE_ACTIVE;
    
    // Emite evento canônico
    v3_events_post((resource_id_t)key->raw, RESOURCE_EVT_ONLINE);
    
    return true;
}

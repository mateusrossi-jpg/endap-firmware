#include "control_kernel.h"

#include "deterministic_snapshot.h"
#include "runtime_monitor.h"

#include <stdbool.h>
#include <stdint.h>

/* ========================================================= */
/*              ESTADO GLOBAL DO SUPERVISOR                  */
/* ========================================================= */

static uint8_t system_fault_active = 0;
static uint32_t last_cycle_time_us = 0;

/* ========================================================= */
/*                   INICIALIZAÇÃO                           */
/* ========================================================= */

void control_kernel_init(void)
{
    /* ownership oficial do runtime_monitor_init() */
    runtime_monitor_init();

    /* placeholder controlado para extensão futura */
    deterministic_snapshot_init();

    system_fault_active = 0;
    last_cycle_time_us = 0;
}

/* ========================================================= */
/*                 CICLO PRINCIPAL DO KERNEL                 */
/* ========================================================= */

void control_kernel_task(void)
{
    /* ============================================= */
    /* SNAPSHOT DETERMINÍSTICO                       */
    /* ============================================= */

    deterministic_snapshot_tick();

    /* ============================================= */
    /* MONITOR DE RUNTIME                            */
    /* ============================================= */

    runtime_monitor_tick();

    last_cycle_time_us = runtime_monitor_get_last_cycle_us();
    system_fault_active = runtime_monitor_is_fault();
}

/* ========================================================= */
/*              API PÚBLICA DO SUPERVISOR                   */
/* ========================================================= */

uint8_t control_kernel_has_fault(void)
{
    return system_fault_active;
}

uint32_t control_kernel_get_last_cycle_us(void)
{
    return last_cycle_time_us;
}

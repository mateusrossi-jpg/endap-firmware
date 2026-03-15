#include "control_kernel.h"

#include "deterministic_snapshot.h"
#include "watchdog.h"

#include "runtime_monitor.h"
#include "watchdog_ids.h"

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
    runtime_monitor_init();
    deterministic_snapshot_init();

    system_fault_active = 0;
}

/* ========================================================= */
/*                 CICLO PRINCIPAL DO KERNEL                 */
/* ========================================================= */

void control_kernel_task(void)
{
    /* ============================================= */
    /* WATCHDOG                                      */
    /* ============================================= */

    watchdog_feed(WD_CONTROL_LOOP);

    /* ============================================= */
    /* SNAPSHOT DETERMINÍSTICO                       */
    /* ============================================= */

    deterministic_snapshot_tick();

    /* ============================================= */
    /* MONITOR DE RUNTIME                            */
    /* ============================================= */

    runtime_monitor_tick();
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

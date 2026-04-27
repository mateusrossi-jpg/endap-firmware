#include "scheduler.h"
#include "esp_attr.h"

#include "io_driver.h"
#include "io_image.h"
#include "automation_engine.h"
#include "fieldbus.h"
#include "control_kernel.h"
#include "event_bus.h"
#include "io_command.h"
#include "state.h"

/* ============================================================
   CONFIG (🔥 CONTROLE HARD REAL-TIME)
============================================================ */

#define IO_CMD_BUDGET      1
#define EVENT_BUS_BUDGET   1

/* ============================================================
   IO PHASE (🔥 PROCESSAMENTO - LEVE)
============================================================ */

void IRAM_ATTR scheduler_run_io(void)
{
    io_driver_scan_inputs();
    io_image_scan_inputs();

    io_cmd_t cmd;
    int budget = IO_CMD_BUDGET;

    while (budget-- && io_command_pop(&cmd))
    {
        state_set_int(cmd.id, cmd.value);
    }
}

/* ============================================================
   IO APPLY PHASE (🔥 ATUAÇÃO - SEPARADA)
============================================================ */

void IRAM_ATTR scheduler_run_io_apply(void)
{
    io_driver_update();
}

/* ============================================================
   FIELDBUS (🔥 DETERMINÍSTICO)
============================================================ */

void IRAM_ATTR scheduler_run_fieldbus(void)
{
    fieldbus_tick();
}

/* ============================================================
   AUTOMATION
============================================================ */

void IRAM_ATTR scheduler_run_automation(void)
{
    automation_engine_tick_1ms();
    control_kernel_task();
}

/* ============================================================
   EVENTS (🔥 LEVE E LIMITADO)
============================================================ */

void IRAM_ATTR scheduler_run_events(void)
{
    event_bus_dispatch_budgeted(EVENT_BUS_BUDGET);
}

/* ============================================================
   DIAGNOSTICS (🔥 FORA DO CRÍTICO)
============================================================ */

void IRAM_ATTR scheduler_run_diagnostics(void)
{
    /* diagnostics processados por task auxiliar em core 0 */
}

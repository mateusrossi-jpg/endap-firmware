#include "phase_monitor.h"

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/portmacro.h"

#include <inttypes.h>

#define TAG "PHASE_MON"

#define PHASE_COUNT 6   // 🔥 agora você tem IO + IO_APPLY

/* 🔥 DEADLINES AJUSTADOS (REALISTAS) */
static const uint32_t phase_deadline[PHASE_COUNT] =
{
    70,   /* IO (antes 50) */
    30,   /* IO_APPLY (novo, super leve) */
    100,  /* FIELDBUS (com budget já controlado) */
    150,  /* AUTOMATION */
    50,   /* EVENTS */
    100   /* DIAGNOSTICS */
};

/* 🔥 CONTROLE DE LOG (anti flood + determinismo) */
static uint32_t overrun_counter[PHASE_COUNT] = {0};
static portMUX_TYPE phase_monitor_mux = portMUX_INITIALIZER_UNLOCKED;

void phase_monitor_check(
    uint8_t phase,
    uint32_t exec_time)
{
    uint32_t count;

    if (phase >= PHASE_COUNT)
        return;

    if (exec_time > phase_deadline[phase])
    {
        portENTER_CRITICAL(&phase_monitor_mux);
        overrun_counter[phase]++;
        count = overrun_counter[phase];
        portEXIT_CRITICAL(&phase_monitor_mux);

        /* 🔥 log a cada 100 ocorrências (evita jitter) */
        if ((count % 100U) == 0U)
        {
            ESP_LOGW(TAG,
                "phase %" PRIu32 " overrun (%" PRIu32 " us > %" PRIu32 " us) count=%" PRIu32,
                (uint32_t)phase,
                exec_time,
                phase_deadline[phase],
                count);
        }
    }
}

void phase_monitor_get(phase_monitor_snapshot_t *snapshot)
{
    if (!snapshot)
        return;

    portENTER_CRITICAL(&phase_monitor_mux);

    snapshot->io_deadline_us = phase_deadline[0];
    snapshot->io_apply_deadline_us = phase_deadline[1];
    snapshot->fieldbus_deadline_us = phase_deadline[2];
    snapshot->automation_deadline_us = phase_deadline[3];
    snapshot->events_deadline_us = phase_deadline[4];
    snapshot->diagnostics_deadline_us = phase_deadline[5];

    snapshot->io_overruns = overrun_counter[0];
    snapshot->io_apply_overruns = overrun_counter[1];
    snapshot->fieldbus_overruns = overrun_counter[2];
    snapshot->automation_overruns = overrun_counter[3];
    snapshot->events_overruns = overrun_counter[4];
    snapshot->diagnostics_overruns = overrun_counter[5];

    portEXIT_CRITICAL(&phase_monitor_mux);
}

#include "phase_monitor.h"
#include "esp_log.h"
#include <inttypes.h>

#define TAG "PHASE_MON"

#define PHASE_COUNT 5

static const uint32_t phase_deadline[PHASE_COUNT] =
{
    50,   /* IO */
    300,  /* FIELDBUS */
    150,  /* AUTOMATION */
    50,   /* EVENTS */
    50    /* DIAGNOSTICS */
};

void phase_monitor_check(
    uint8_t phase,
    uint32_t exec_time)
{
    if (phase >= PHASE_COUNT)
        return;

    if (exec_time > phase_deadline[phase])
    {
        ESP_LOGW(TAG,
        "phase %" PRIu32 " overrun (%" PRIu32 " us > %" PRIu32 " us)",
        (uint32_t)phase,
        exec_time,
        phase_deadline[phase]);
    }
}

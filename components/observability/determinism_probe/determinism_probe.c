#include "determinism_probe.h"
#include "esp_log.h"

#include <inttypes.h>
#include <limits.h>

#define TAG "DET_PROBE"

static uint64_t start_time;
static uint32_t cycles;

static uint32_t min = UINT32_MAX;
static uint32_t max = 0;
static uint64_t sum = 0;

/* ============================================================
   CYCLE START
============================================================ */

void IRAM_ATTR determinism_probe_cycle_start(uint64_t now)
{
    start_time = now;
}

/* ============================================================
   CYCLE END
============================================================ */

void IRAM_ATTR determinism_probe_cycle_end(uint64_t now)
{
    uint64_t dt;

    if (now >= start_time)
        dt = now - start_time;
    else
        dt = 0;

    if (dt < min) min = dt;
    if (dt > max) max = dt;

    sum += dt;

    cycles++;

    if ((cycles & 1023) == 0)
    {
        ESP_LOGI(TAG,
            "cycles=%" PRIu32
            " jitter(min/avg/max)=%" PRIu32 "/%" PRIu32 "/%" PRIu32 " us",
            cycles,
            min,
            (uint32_t)(sum / cycles),
            max);
    }
}

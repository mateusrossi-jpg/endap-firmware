#include "determinism_probe.h"
#include "esp_log.h"

#include <inttypes.h>
#include <limits.h>

#define TAG "DET_PROBE"

static uint64_t start;
static uint32_t cycles;

static uint32_t min = UINT32_MAX;
static uint32_t max = 0;
static uint64_t sum = 0;

void IRAM_ATTR determinism_probe_cycle_start(uint64_t now)
{
    start = now;
}

void IRAM_ATTR determinism_probe_cycle_end(uint64_t now)
{
    uint32_t dt = (uint32_t)(now - start);

    if (dt < min) min = dt;
    if (dt > max) max = dt;

    sum += dt;

    cycles++;

    if ((cycles & 1023) == 0)
    {
        ESP_LOGI(TAG,
        "cycles=%" PRIu32 " jitter(min/avg/max)=%" PRIu32 "/%" PRIu32 "/%" PRIu32 " us",
        cycles,
        min,
        (uint32_t)(sum / cycles),
        max);
    }
}

#include "kernel_trace.h"
#include "esp_log.h"

#include <inttypes.h>
#include <limits.h>

#define TAG "KTRACE"
#define LOOP_PERIOD_US 1000

static uint64_t cycle_start;
static uint64_t last_cycle = 0;

static uint32_t cycles = 0;

static uint32_t jitter_min = UINT32_MAX;
static uint32_t jitter_max = 0;
static uint64_t jitter_sum = 0;

static uint32_t exec_min = UINT32_MAX;
static uint32_t exec_max = 0;
static uint64_t exec_sum = 0;

/* ============================================================
   CYCLE START
============================================================ */

void IRAM_ATTR kernel_trace_cycle_start(uint64_t now)
{
    cycle_start = now;

    if (last_cycle != 0)
    {
        uint64_t period;

        if (now >= last_cycle)
            period = now - last_cycle;
        else
            period = 0;

        if (period < 100000)
        {
            uint32_t jitter =
                (period > LOOP_PERIOD_US)
                ? (period - LOOP_PERIOD_US)
                : (LOOP_PERIOD_US - period);

            if (jitter < jitter_min)
                jitter_min = jitter;

            if (jitter > jitter_max)
                jitter_max = jitter;

            jitter_sum += jitter;
        }
    }

    last_cycle = now;
}

/* ============================================================
   CYCLE END
============================================================ */

void IRAM_ATTR kernel_trace_cycle_end(uint64_t now)
{
    uint64_t exec;

    if (now >= cycle_start)
        exec = now - cycle_start;
    else
        exec = 0;

    if (exec < exec_min)
        exec_min = exec;

    if (exec > exec_max)
        exec_max = exec;

    exec_sum += exec;

    cycles++;

    if ((cycles & 1023) == 0)
    {
        ESP_LOGI(TAG,
            "cycles=%" PRIu32
            " jitter(min/avg/max)=%" PRIu32 "/%" PRIu32 "/%" PRIu32 " us"
            " exec(min/avg/max)=%" PRIu32 "/%" PRIu32 "/%" PRIu32 " us",
            cycles,
            jitter_min,
            (uint32_t)(jitter_sum / cycles),
            jitter_max,
            exec_min,
            (uint32_t)(exec_sum / cycles),
            exec_max);
    }
}

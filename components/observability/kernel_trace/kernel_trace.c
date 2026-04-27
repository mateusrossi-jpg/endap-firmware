#include "kernel_trace.h"
#include "esp_attr.h"
#include "esp_log.h"

#include "freertos/FreeRTOS.h"
#include "freertos/portmacro.h"

#include <inttypes.h>
#include <limits.h>
#include <stdbool.h>

#define TAG "KTRACE"
#define LOOP_PERIOD_US 1000
#define REPORT_WINDOW_CYCLES 1024

typedef struct
{
    uint32_t cycles;
    uint32_t jitter_min;
    uint32_t jitter_avg;
    uint32_t jitter_max;
    uint32_t exec_min;
    uint32_t exec_avg;
    uint32_t exec_max;
    uint32_t slip_events;
    uint32_t missed_cycles;
    uint32_t slip_max;
} kernel_trace_report_t;

static uint64_t cycle_start;
static uint64_t last_cycle = 0;

static uint32_t cycles = 0;
static uint32_t jitter_samples = 0;
static uint32_t exec_samples = 0;
static uint32_t slip_events = 0;
static uint32_t missed_cycles = 0;

static uint32_t jitter_min = UINT32_MAX;
static uint32_t jitter_max = 0;
static uint64_t jitter_sum = 0;
static uint32_t slip_max = 0;

static uint32_t exec_min = UINT32_MAX;
static uint32_t exec_max = 0;
static uint64_t exec_sum = 0;

static kernel_trace_report_t pending_report;
static bool report_pending = false;

static portMUX_TYPE trace_mux = portMUX_INITIALIZER_UNLOCKED;

static void reset_window(void)
{
    jitter_min = UINT32_MAX;
    jitter_max = 0;
    jitter_sum = 0;
    jitter_samples = 0;
    slip_events = 0;
    missed_cycles = 0;
    slip_max = 0;

    exec_min = UINT32_MAX;
    exec_max = 0;
    exec_sum = 0;
    exec_samples = 0;
}

void kernel_trace_init(void)
{
    cycle_start = 0;
    last_cycle = 0;
    cycles = 0;
    report_pending = false;
    reset_window();
}

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
            if (period < (2 * LOOP_PERIOD_US))
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
                jitter_samples++;
            }
            else
            {
                uint32_t slip = (uint32_t)(period - LOOP_PERIOD_US);
                uint32_t missed = (uint32_t)(period / LOOP_PERIOD_US);

                if (missed > 0)
                    missed--;

                if (slip > slip_max)
                    slip_max = slip;

                slip_events++;
                missed_cycles += missed;
            }
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
    exec_samples++;

    cycles++;

    if ((cycles % REPORT_WINDOW_CYCLES) == 0 && exec_samples)
    {
        kernel_trace_report_t report = {
            .cycles = cycles,
            .jitter_min = jitter_samples ? jitter_min : 0,
            .jitter_avg = jitter_samples ? (uint32_t)(jitter_sum / jitter_samples) : 0,
            .jitter_max = jitter_samples ? jitter_max : 0,
            .exec_min = exec_min,
            .exec_avg = (uint32_t)(exec_sum / exec_samples),
            .exec_max = exec_max,
            .slip_events = slip_events,
            .missed_cycles = missed_cycles,
            .slip_max = slip_max
        };

        portENTER_CRITICAL(&trace_mux);
        pending_report = report;
        report_pending = true;
        portEXIT_CRITICAL(&trace_mux);

        reset_window();
    }
}

void kernel_trace_process(void)
{
    kernel_trace_report_t report = {0};
    bool should_log = false;

    portENTER_CRITICAL(&trace_mux);

    if (report_pending)
    {
        report = pending_report;
        report_pending = false;
        should_log = true;
    }

    portEXIT_CRITICAL(&trace_mux);

    if (!should_log)
        return;

    ESP_LOGI(TAG,
        "cycles=%" PRIu32
        " jitter(min/avg/max)=%" PRIu32 "/%" PRIu32 "/%" PRIu32 " us"
        " exec(min/avg/max)=%" PRIu32 "/%" PRIu32 "/%" PRIu32 " us"
        " slips=%" PRIu32 " missed=%" PRIu32 " slip_max=%" PRIu32 " us",
        report.cycles,
        report.jitter_min,
        report.jitter_avg,
        report.jitter_max,
        report.exec_min,
        report.exec_avg,
        report.exec_max,
        report.slip_events,
        report.missed_cycles,
        report.slip_max);
}

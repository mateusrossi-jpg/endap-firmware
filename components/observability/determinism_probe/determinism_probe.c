#include "determinism_probe.h"
#include "esp_log.h"

#include "freertos/FreeRTOS.h"
#include "freertos/portmacro.h"

#include <inttypes.h>
#include <limits.h>
#include <stdbool.h>

#define TAG "DET_PROBE"
#define REPORT_WINDOW_CYCLES 1024

typedef struct
{
    uint32_t cycles;
    uint32_t exec_min;
    uint32_t exec_avg;
    uint32_t exec_max;
} determinism_probe_report_t;

static uint64_t start_time;
static uint32_t cycles;
static uint32_t window_samples;

static uint32_t exec_min = UINT32_MAX;
static uint32_t exec_max = 0;
static uint64_t exec_sum = 0;

static determinism_probe_report_t pending_report;
static bool report_pending = false;

static portMUX_TYPE probe_mux = portMUX_INITIALIZER_UNLOCKED;

static void reset_window(void)
{
    exec_min = UINT32_MAX;
    exec_max = 0;
    exec_sum = 0;
    window_samples = 0;
}

void determinism_probe_init(void)
{
    start_time = 0;
    cycles = 0;
    report_pending = false;
    reset_window();
}

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

    if (dt < exec_min) exec_min = dt;
    if (dt > exec_max) exec_max = dt;

    exec_sum += dt;
    window_samples++;
    cycles++;

    if ((cycles % REPORT_WINDOW_CYCLES) == 0 && window_samples)
    {
        determinism_probe_report_t report = {
            .cycles = cycles,
            .exec_min = exec_min,
            .exec_avg = (uint32_t)(exec_sum / window_samples),
            .exec_max = exec_max
        };

        portENTER_CRITICAL(&probe_mux);
        pending_report = report;
        report_pending = true;
        portEXIT_CRITICAL(&probe_mux);

        reset_window();
    }
}

void determinism_probe_process(void)
{
    determinism_probe_report_t report = {0};
    bool should_log = false;

    portENTER_CRITICAL(&probe_mux);

    if (report_pending)
    {
        report = pending_report;
        report_pending = false;
        should_log = true;
    }

    portEXIT_CRITICAL(&probe_mux);

    if (!should_log)
        return;

    ESP_LOGI(TAG,
        "cycles=%" PRIu32 " exec(min/avg/max)=%" PRIu32 "/%" PRIu32 "/%" PRIu32 " us",
        report.cycles,
        report.exec_min,
        report.exec_avg,
        report.exec_max);
}

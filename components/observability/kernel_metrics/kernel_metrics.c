#include "kernel_metrics.h"

#include <inttypes.h>
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/portmacro.h"

#define TAG "KERNEL_METRICS"

/* ============================================================
   STATE (🔥 PROTEGIDO)
============================================================ */

static uint64_t jitter_max;
static uint64_t exec_max;

static uint32_t deadline_miss;
static uint32_t overrun;

static uint32_t cycle_counter;

/* spinlock leve (ISR safe) */
static portMUX_TYPE metrics_mux = portMUX_INITIALIZER_UNLOCKED;

/* ============================================================
   INIT
============================================================ */

void kernel_metrics_init(void)
{
    portENTER_CRITICAL(&metrics_mux);

    jitter_max = 0;
    exec_max = 0;
    deadline_miss = 0;
    overrun = 0;
    cycle_counter = 0;

    portEXIT_CRITICAL(&metrics_mux);
}

/* ============================================================
   UPDATE (🔥 CHAMADO NO LOOP)
============================================================ */

void kernel_metrics_update(
    uint64_t jitter,
    uint64_t exec,
    uint32_t miss,
    uint32_t over)
{
    portENTER_CRITICAL(&metrics_mux);

    jitter_max = jitter;
    exec_max = exec;
    deadline_miss = miss;
    overrun = over;

    cycle_counter++;

    bool do_log = false;

    if (cycle_counter >= 5000)
    {
        do_log = true;
        cycle_counter = 0;
    }

    portEXIT_CRITICAL(&metrics_mux);

    /* 🔥 log fora da região crítica */
    if (do_log)
    {
        kernel_metrics_log();
    }
}

/* ============================================================
   GET (🔥 ESSENCIAL)
============================================================ */

void kernel_metrics_get(kernel_metrics_snapshot_t *m)
{
    portENTER_CRITICAL(&metrics_mux);

    m->max_jitter = jitter_max;
    m->max_exec_time = exec_max;
    m->deadline_miss = deadline_miss;
    m->overrun_count = overrun;

    portEXIT_CRITICAL(&metrics_mux);
}

/* ============================================================
   LOG
============================================================ */

void kernel_metrics_log(void)
{
    uint64_t j, e;
    uint32_t m, o;

    /* snapshot seguro */
    portENTER_CRITICAL(&metrics_mux);

    j = jitter_max;
    e = exec_max;
    m = deadline_miss;
    o = overrun;

    portEXIT_CRITICAL(&metrics_mux);

    ESP_LOGI(TAG,
        "jitter_max=%" PRIu64 " us exec_max=%" PRIu64 " us deadline_miss=%" PRIu32 " overrun=%" PRIu32,
        j, e, m, o
    );
}

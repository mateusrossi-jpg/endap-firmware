#include "kernel_metrics.h"

#include <inttypes.h>
#include "esp_log.h"

#define TAG "KERNEL_METRICS"

static uint64_t jitter_max;
static uint64_t exec_max;

static uint32_t deadline_miss;
static uint32_t overrun;

static uint32_t cycle_counter;

void kernel_metrics_init(void)
{
    jitter_max = 0;
    exec_max = 0;

    deadline_miss = 0;
    overrun = 0;

    cycle_counter = 0;
}

void kernel_metrics_update(
    uint64_t jitter,
    uint64_t exec,
    uint32_t miss,
    uint32_t over)
{
    jitter_max = jitter;
    exec_max = exec;

    deadline_miss = miss;
    overrun = over;

    cycle_counter++;

    /* log every ~5 seconds */

    if(cycle_counter >= 5000)
    {
        kernel_metrics_log();
        cycle_counter = 0;
    }
    
}

void kernel_metrics_log(void)
{
    ESP_LOGI(TAG,
        "jitter_max=%" PRIu64 " us exec_max=%" PRIu64 " us deadline_miss=%" PRIu32 " overrun=%" PRIu32,
        jitter_max,
        exec_max,
        deadline_miss,
        overrun
    );
}

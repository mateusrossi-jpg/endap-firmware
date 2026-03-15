#include "control_loop.h"

#include <inttypes.h>
#include <limits.h>

#include "esp_log.h"
#include "esp_system.h"

#include "driver/gptimer.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "scheduler.h"
#include "watchdog.h"
#include "watchdog_ids.h"

#include "determinism_probe.h"
#include "kernel_trace.h"
#include "kernel_metrics.h"
#include "phase_monitor.h"

#define TAG "CTRL_LOOP"

#define LOOP_PERIOD_US 1000
#define LOOP_WARN_US   3000
#define BOOT_IGNORE_CYCLES 2000

#define DEADLINE_FAULT_THRESHOLD 200

/* ============================================================
   STATIC TASK (NO HEAP)
============================================================ */

static StackType_t control_stack[4096];
static StaticTask_t control_task_buffer;

/* ============================================================
   TIMER
============================================================ */

static gptimer_handle_t loop_timer = NULL;
static TaskHandle_t control_task_handle = NULL;

/* ============================================================
   RUNTIME STATE
============================================================ */

static uint64_t expected_cycle_time = 0;
static uint32_t cycle_counter = 0;

static uint64_t jitter_sum = 0;
static uint32_t jitter_samples = 0;

static uint64_t jitter_min = UINT64_MAX;
static uint64_t jitter_max = 0;

static uint64_t exec_min = UINT64_MAX;
static uint64_t exec_max = 0;

static uint32_t deadline_miss = 0;
static uint32_t overrun_count = 0;

/* ============================================================
   PHASE METRICS
============================================================ */

static uint32_t phase_io_max = 0;
static uint32_t phase_fieldbus_max = 0;
static uint32_t phase_automation_max = 0;
static uint32_t phase_events_max = 0;
static uint32_t phase_diag_max = 0;

static uint32_t *phase_max_table[] =
{
    &phase_io_max,
    &phase_fieldbus_max,
    &phase_automation_max,
    &phase_events_max,
    &phase_diag_max
};

/* ============================================================
   SCHEDULER TABLE (CONST = DETERMINISTIC)
============================================================ */

typedef void (*phase_fn_t)(void);

static const phase_fn_t phase_table[] =
{
    scheduler_run_io,
    scheduler_run_fieldbus,
    scheduler_run_automation,
    scheduler_run_events,
    scheduler_run_diagnostics
};

#define PHASE_COUNT (sizeof(phase_table) / sizeof(phase_fn_t))

/* ============================================================
   TIMER ISR
============================================================ */

static bool IRAM_ATTR timer_callback(
    gptimer_handle_t timer,
    const gptimer_alarm_event_data_t *edata,
    void *user_ctx)
{
    BaseType_t hp = pdFALSE;

    vTaskNotifyGiveFromISR(control_task_handle, &hp);

    return hp == pdTRUE;
}

/* ============================================================
   FAST TIMER READ
============================================================ */

static inline IRAM_ATTR uint64_t read_time(void)
{
    uint64_t t;
    gptimer_get_raw_count(loop_timer, &t);
    return t;
}

/* ============================================================
   CONTROL LOOP TASK
============================================================ */

static void IRAM_ATTR control_loop_run(void *arg)
{
    while (1)
    {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

        while (ulTaskNotifyTake(pdTRUE, 0))
            overrun_count++;

        uint64_t start = read_time();

        if (expected_cycle_time == 0)
            expected_cycle_time = start;

        int64_t jitter = (int64_t)start - (int64_t)expected_cycle_time;

        if (jitter < 0)
            jitter = -jitter;

        if (jitter > 1000000)
        {
            expected_cycle_time = start + LOOP_PERIOD_US;
            continue;
        }

        cycle_counter++;

        if (cycle_counter > BOOT_IGNORE_CYCLES)
        {
            uint64_t j = (uint64_t)jitter;

            if (j < jitter_min) jitter_min = j;
            if (j > jitter_max) jitter_max = j;

            jitter_sum += j;
            jitter_samples++;
        }

        if (start > expected_cycle_time)
        {
            uint64_t diff = start - expected_cycle_time;
            expected_cycle_time += ((diff / LOOP_PERIOD_US) + 1) * LOOP_PERIOD_US;
        }

        determinism_probe_cycle_start(start);
        kernel_trace_cycle_start(start);

        /* ============================================================
           PHASE EXECUTION
        ============================================================ */

        for (uint8_t i = 0; i < PHASE_COUNT; i++)
        {
            uint64_t t_before = read_time();

            phase_table[i]();

            uint64_t t_after = read_time();

            uint64_t dt;

            if (t_after >= t_before)
                dt = t_after - t_before;
            else
                dt = 0;

            if (dt > *phase_max_table[i])
                *phase_max_table[i] = (uint32_t)dt;

            phase_monitor_check(i, (uint32_t)dt);
        }

        uint64_t end = read_time();

        uint64_t exec;

        if (end >= start)
            exec = end - start;
        else
            exec = 0;

        if (exec < exec_min) exec_min = exec;
        if (exec > exec_max) exec_max = exec;

        if (exec > LOOP_PERIOD_US)
        {
            deadline_miss++;

            ESP_LOGE(TAG,
                "deadline miss exec=%" PRIu64 " us",
                exec);

            if (deadline_miss > DEADLINE_FAULT_THRESHOLD)
            {
                ESP_LOGE(TAG,"KERNEL FAULT: too many deadline misses");
                esp_system_abort("control loop deadline fault");
            }
        }

        determinism_probe_cycle_end(end);
        kernel_trace_cycle_end(end);

        if (jitter_samples && (cycle_counter % 5000) == 0)
        {
            uint64_t avg = jitter_sum / jitter_samples;

            kernel_metrics_update(
                jitter_max,
                exec_max,
                deadline_miss,
                overrun_count
            );

            ESP_LOGI(TAG,
                "jitter(min/avg/max)=%" PRIu64 "/%" PRIu64 "/%" PRIu64
                " exec(min/max)=%" PRIu64 "/%" PRIu64,
                jitter_min,
                avg,
                jitter_max,
                exec_min,
                exec_max);
        }

        watchdog_feed(WD_CONTROL_LOOP);
    }
}

/* ============================================================
   CONTROL LOOP START
============================================================ */

void control_loop_start(void)
{
    ESP_LOGI(TAG,"Initializing control loop");

    gptimer_config_t timer_config =
    {
        .clk_src = GPTIMER_CLK_SRC_DEFAULT,
        .direction = GPTIMER_COUNT_UP,
        .resolution_hz = 1000000
    };

    ESP_ERROR_CHECK(gptimer_new_timer(&timer_config, &loop_timer));

    gptimer_event_callbacks_t cbs =
    {
        .on_alarm = timer_callback
    };

    ESP_ERROR_CHECK(gptimer_register_event_callbacks(loop_timer, &cbs, NULL));

    gptimer_alarm_config_t alarm_config =
    {
        .alarm_count = LOOP_PERIOD_US,
        .reload_count = 0,
        .flags.auto_reload_on_alarm = true
    };

    ESP_ERROR_CHECK(gptimer_set_alarm_action(loop_timer, &alarm_config));

    control_task_handle = xTaskCreateStaticPinnedToCore(
        control_loop_run,
        "control_loop",
        4096,
        NULL,
        configMAX_PRIORITIES - 1,
        control_stack,
        &control_task_buffer,
        1
    );

    ESP_ERROR_CHECK(gptimer_enable(loop_timer));
    ESP_ERROR_CHECK(gptimer_start(loop_timer));

    ESP_LOGI(TAG,"Control loop started (1ms)");
}

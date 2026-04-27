#include "fieldbus.h"

#include "rs485_master.h"
#include "rs485_engine.h"
#include "device_profile.h"
#include "esp_timer.h"
#include "esp_attr.h"
#include "esp_log.h"

#include "bus_health_monitor.h"
#include "latency_histogram.h"

#include <inttypes.h>
#include <string.h>

/* ============================================================
   CONFIG
============================================================ */

#define FIELDBUS_BUDGET_US            80
#define FIELDBUS_PROFILE_LOG_CYCLES   2000U

/* ============================================================
   PROFILE
============================================================ */

#define FIELD_PROFILE_TAG "FIELDBUS_PROF"
#define TAG               "FIELDBUS"

typedef struct
{
    uint32_t cycles;

    uint32_t engine_sum_us;
    uint32_t engine_max_us;

    uint32_t master_calls;
    uint32_t master_sum_us;
    uint32_t master_max_us;

    uint32_t event_calls;
    uint32_t event_sum_us;
    uint32_t event_max_us;

    uint32_t total_sum_us;
    uint32_t total_max_us;
} fieldbus_profile_acc_t;

typedef struct
{
    uint32_t cycles;

    uint32_t engine_avg_us;
    uint32_t engine_max_us;

    uint32_t master_calls;
    uint32_t master_avg_us;
    uint32_t master_max_us;

    uint32_t event_calls;
    uint32_t event_avg_us;
    uint32_t event_max_us;

    uint32_t total_avg_us;
    uint32_t total_max_us;
} fieldbus_profile_snapshot_t;

/* ============================================================
   STATE
============================================================ */

static uint64_t bus_last_activity = 0;
static uint32_t histogram_cycle = 0;
static volatile bool histogram_log_request = false;

static fieldbus_profile_acc_t fieldbus_profile = {0};
static volatile bool fieldbus_profile_log_request = false;
static bool rs485_runtime_enabled = false;
static bool rs485_master_enabled = false;

/* ============================================================
   HELPERS
============================================================ */

static inline uint32_t IRAM_ATTR time_diff_us(uint64_t start, uint64_t end)
{
    return (uint32_t)((end >= start) ? (end - start) : 0U);
}

static inline void IRAM_ATTR profile_accumulate(uint32_t *sum, uint32_t *max, uint32_t dt)
{
    *sum += dt;

    if (dt > *max)
        *max = dt;
}

static void fieldbus_profile_reset(void)
{
    memset(&fieldbus_profile, 0, sizeof(fieldbus_profile));
}

static void fieldbus_profile_snapshot(fieldbus_profile_snapshot_t *out, bool reset)
{
    if (!out)
        return;

    memset(out, 0, sizeof(*out));

    out->cycles = fieldbus_profile.cycles;

    out->engine_max_us = fieldbus_profile.engine_max_us;
    out->master_calls = fieldbus_profile.master_calls;
    out->master_max_us = fieldbus_profile.master_max_us;
    out->event_calls = fieldbus_profile.event_calls;
    out->event_max_us = fieldbus_profile.event_max_us;
    out->total_max_us = fieldbus_profile.total_max_us;

    if (fieldbus_profile.cycles)
    {
        out->engine_avg_us = fieldbus_profile.engine_sum_us / fieldbus_profile.cycles;
        out->total_avg_us = fieldbus_profile.total_sum_us / fieldbus_profile.cycles;
    }

    if (fieldbus_profile.master_calls)
        out->master_avg_us = fieldbus_profile.master_sum_us / fieldbus_profile.master_calls;

    if (fieldbus_profile.event_calls)
        out->event_avg_us = fieldbus_profile.event_sum_us / fieldbus_profile.event_calls;

    if (reset)
        fieldbus_profile_reset();
}

static bool fieldbus_should_run_rs485_master(void)
{
    if (!device_profile_transport_enabled(DEVICE_PROFILE_TRANSPORT_RS485))
        return false;

    /*
     * Regra v1:
     * - nó com transporte IP habilitado (Ethernet ou Wi‑Fi) pode atuar como gateway/mestre do barramento
     * - nó RS485-only permanece passivo/respondedor
     */
    return device_profile_transport_enabled(DEVICE_PROFILE_TRANSPORT_ETHERNET) ||
           device_profile_transport_enabled(DEVICE_PROFILE_TRANSPORT_WIFI);
}

/* ============================================================
   INIT
============================================================ */

void fieldbus_init(void)
{
    rs485_runtime_enabled = false;
    rs485_master_enabled = false;
    histogram_cycle = 0U;
    histogram_log_request = false;
    fieldbus_profile_log_request = false;
    latency_histogram_init();
    bus_health_init();
    bus_last_activity = (uint64_t)esp_timer_get_time();
    fieldbus_profile_reset();

    if (!device_profile_transport_enabled(DEVICE_PROFILE_TRANSPORT_RS485))
    {
        ESP_LOGI(TAG, "RS485 desabilitado no perfil -> fieldbus fica inativo");
        return;
    }

    rs485_runtime_enabled = true;
    rs485_master_enabled = fieldbus_should_run_rs485_master();

    if (rs485_master_enabled)
    {
        rs485_master_init();
        ESP_LOGI(TAG, "RS485 role=master (gateway / multi-transport)");
    }
    else
    {
        rs485_engine_init();
        rs485_engine_enable();
        ESP_LOGI(TAG, "RS485 role=passive (field node / responder)");
    }
}

/* ============================================================
   WATCHDOG
============================================================ */

static inline IRAM_ATTR void bus_watchdog(uint64_t now)
{
    if ((now - bus_last_activity) > 2000000ULL)
        bus_last_activity = now;
}

static inline void fieldbus_handle_event(const rs485_master_event_t *event, uint64_t now)
{
    if (!event)
        return;

    switch (event->type)
    {
        case RS485_MASTER_EVENT_TX:
            bus_last_activity = now;
            break;

        case RS485_MASTER_EVENT_ACK:
            bus_last_activity = now;
            latency_histogram_record(event->latency_us);
            bus_health_latency(event->latency_us);
            break;

        case RS485_MASTER_EVENT_RETRY:
            bus_last_activity = now;
            bus_health_retry();
            break;

        case RS485_MASTER_EVENT_TIMEOUT:
            bus_last_activity = now;
            bus_health_timeout();
            break;

        case RS485_MASTER_EVENT_CRC_ERROR:
            bus_last_activity = now;
            bus_health_crc_error();
            break;

        case RS485_MASTER_EVENT_RX_IGNORED:
        case RS485_MASTER_EVENT_NONE:
        default:
            break;
    }
}

static inline void fieldbus_handle_passive_rx(const rs485_engine_rx_result_t rx, uint64_t now)
{
    switch (rx)
    {
        case RS485_ENGINE_RX_FRAME:
            bus_last_activity = now;
            break;
        case RS485_ENGINE_RX_CRC_ERROR:
            bus_last_activity = now;
            bus_health_crc_error();
            break;
        case RS485_ENGINE_RX_FORMAT_ERROR:
            bus_last_activity = now;
            break;
        case RS485_ENGINE_RX_NONE:
        default:
            break;
    }
}

/* ============================================================
   TICK
============================================================ */

void IRAM_ATTR fieldbus_tick(void)
{
    uint64_t total_start = (uint64_t)esp_timer_get_time();
    uint64_t t0;
    uint64_t t1;

    rs485_master_event_t event = {0};
    bool event_processed = false;

    if (!rs485_runtime_enabled)
        return;

    t0 = (uint64_t)esp_timer_get_time();
    rs485_engine_tick_1ms();
    t1 = (uint64_t)esp_timer_get_time();
    profile_accumulate(&fieldbus_profile.engine_sum_us,
                       &fieldbus_profile.engine_max_us,
                       time_diff_us(t0, t1));

    if (rs485_master_enabled)
    {
        if (rs485_master_has_work())
        {
            t0 = (uint64_t)esp_timer_get_time();
            bool handled = rs485_master_process_one(&event);
            t1 = (uint64_t)esp_timer_get_time();

            fieldbus_profile.master_calls++;
            profile_accumulate(&fieldbus_profile.master_sum_us,
                               &fieldbus_profile.master_max_us,
                               time_diff_us(t0, t1));

            if (handled)
            {
                t0 = (uint64_t)esp_timer_get_time();
                fieldbus_handle_event(&event, (uint64_t)esp_timer_get_time());
                t1 = (uint64_t)esp_timer_get_time();

                fieldbus_profile.event_calls++;
                profile_accumulate(&fieldbus_profile.event_sum_us,
                                   &fieldbus_profile.event_max_us,
                                   time_diff_us(t0, t1));

                event_processed = true;
            }
        }
    }
    else
    {
        rs485_frame_t frame = {0};
        t0 = (uint64_t)esp_timer_get_time();
        rs485_engine_rx_result_t rx = rs485_engine_receive(&frame);
        t1 = (uint64_t)esp_timer_get_time();

        fieldbus_profile.master_calls++;
        profile_accumulate(&fieldbus_profile.master_sum_us,
                           &fieldbus_profile.master_max_us,
                           time_diff_us(t0, t1));

        if (rx != RS485_ENGINE_RX_NONE)
        {
            t0 = (uint64_t)esp_timer_get_time();
            fieldbus_handle_passive_rx(rx, (uint64_t)esp_timer_get_time());
            t1 = (uint64_t)esp_timer_get_time();

            fieldbus_profile.event_calls++;
            profile_accumulate(&fieldbus_profile.event_sum_us,
                               &fieldbus_profile.event_max_us,
                               time_diff_us(t0, t1));
            event_processed = true;
        }
    }

    histogram_cycle++;

    if (histogram_cycle >= 10000U)
    {
        histogram_cycle = 0U;
        histogram_log_request = true;
    }

    fieldbus_profile.cycles++;
    profile_accumulate(&fieldbus_profile.total_sum_us,
                       &fieldbus_profile.total_max_us,
                       time_diff_us(total_start, (uint64_t)esp_timer_get_time()));

    if (fieldbus_profile.cycles >= FIELDBUS_PROFILE_LOG_CYCLES)
        fieldbus_profile_log_request = true;

    bus_watchdog((uint64_t)esp_timer_get_time());

    (void)event_processed;
    (void)FIELDBUS_BUDGET_US;
}

/* ============================================================
   LOG
============================================================ */

void fieldbus_process_logs(void)
{
    if (!rs485_runtime_enabled)
    {
        histogram_log_request = false;
        fieldbus_profile_log_request = false;
        return;
    }

    if (fieldbus_profile_log_request)
    {
        fieldbus_profile_snapshot_t fb = {0};
        rs485_master_profile_metrics_t master = {0};

        fieldbus_profile_log_request = false;

        fieldbus_profile_snapshot(&fb, true);
        if (rs485_master_enabled)
            rs485_master_profile_snapshot(&master, true);
        else
            memset(&master, 0, sizeof(master));

        ESP_LOGI(FIELD_PROFILE_TAG,
                 "fb role=%s cycles=%" PRIu32 " engine(avg/max)=%" PRIu32 "/%" PRIu32
                 " us worker(calls=%" PRIu32 " avg/max)=%" PRIu32 "/%" PRIu32
                 " us event(calls=%" PRIu32 " avg/max)=%" PRIu32 "/%" PRIu32
                 " us total(avg/max)=%" PRIu32 "/%" PRIu32 " us",
                 rs485_master_enabled ? "master" : "passive",
                 fb.cycles,
                 fb.engine_avg_us, fb.engine_max_us,
                 fb.master_calls, fb.master_avg_us, fb.master_max_us,
                 fb.event_calls, fb.event_avg_us, fb.event_max_us,
                 fb.total_avg_us, fb.total_max_us);

        if (rs485_master_enabled)
        {
            ESP_LOGI(FIELD_PROFILE_TAG,
                     "master samples=%" PRIu32 " rx(avg/max)=%" PRIu32 "/%" PRIu32
                     " us frame(calls=%" PRIu32 " avg/max)=%" PRIu32 "/%" PRIu32
                     " us timeout(calls=%" PRIu32 " avg/max)=%" PRIu32 "/%" PRIu32
                     " us tx(calls=%" PRIu32 " avg/max)=%" PRIu32 "/%" PRIu32
                     " us total(avg/max)=%" PRIu32 "/%" PRIu32
                     " us events ack=%" PRIu32 " ignored=%" PRIu32
                     " retry=%" PRIu32 " timeout=%" PRIu32
                     " tx=%" PRIu32 " crc=%" PRIu32,
                     master.samples,
                     master.rx_avg_us, master.rx_max_us,
                     master.frame_events, master.frame_avg_us, master.frame_max_us,
                     master.timeout_events, master.timeout_avg_us, master.timeout_max_us,
                     master.tx_events, master.tx_avg_us, master.tx_max_us,
                     master.total_avg_us, master.total_max_us,
                     master.ack_events, master.ignored_events,
                     master.retry_events, master.timeout_events,
                     master.tx_events, master.crc_events);
        }
    }

    if (histogram_log_request)
    {
        histogram_log_request = false;
        latency_histogram_log();
    }
}

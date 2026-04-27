#include "io_driver.h"
#include "cluster_io.h"
#include "device_profile.h"
#include "failsafe.h"
#include "io_map.h"
#include "io_binding.h"

#include "driver/gpio.h"
#include "esp_attr.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "http_server.h"
#include "io_image.h"
#include "state.h"

#include <inttypes.h>
#include <string.h>

#define TAG "IO_DRIVER"
#define INPUT_DEBOUNCE_SAMPLES 8
#define INPUT_DIAG_PERIOD_US   (5ULL * 1000ULL * 1000ULL)
#define IO_DRIVER_MAX_OUTPUTS  16
#define IO_DRIVER_MAX_INPUTS   16

static uint8_t last_index = 0;
static uint64_t last_diag_report_us = 0;

/* ============================================================
   OUTPUT TABLE
============================================================ */

typedef struct
{
    uint16_t id;
    gpio_num_t gpio;
    bool active_low;
} output_channel_t;

static output_channel_t output_table[IO_DRIVER_MAX_OUTPUTS];
static size_t output_count = 0;

/* ============================================================
   INPUT TABLE
============================================================ */

typedef struct
{
    uint16_t id;
    gpio_num_t gpio;
    bool active_low;
    uint8_t debounce_samples;
    uint8_t last_sample;
    uint8_t stable_value;
    uint8_t stable_count;
    uint32_t raw_edges;
    uint32_t stable_edges;
    uint32_t reported_raw_edges;
    uint32_t reported_stable_edges;
    uint32_t recent_raw_edges;
    uint32_t recent_stable_edges;
    uint32_t recent_noise_edges;
} input_channel_t;

static input_channel_t input_table[IO_DRIVER_MAX_INPUTS];
static size_t input_count = 0;

/* ============================================================
   PENDING MASK (🔥 DETERMINÍSTICO)
============================================================ */

static volatile uint32_t pending_mask = 0;
static uint32_t local_mask = 0;

static uint8_t io_driver_clamp_debounce_samples(uint8_t configured)
{
    if (configured < INPUT_DEBOUNCE_SAMPLES)
        return INPUT_DEBOUNCE_SAMPLES;

    if (configured > 32U)
        return 32U;

    return configured;
}

static inline int IRAM_ATTR io_driver_output_gpio_level(const output_channel_t *ch, bool logical_on)
{
    return ch->active_low ? (logical_on ? 0 : 1) : (logical_on ? 1 : 0);
}

static inline void io_driver_state_shadow_assign(int32_t *shadow, uint16_t id, int32_t value)
{
    if (!shadow)
        return;

    if (id >= STATE_MAX_ENTRIES)
        return;

    shadow[id] = value;
}

static void io_driver_load_profile(void)
{
    io_binding_output_view_t output_views[IO_BINDING_MAX_OUTPUTS];
    io_binding_input_view_t input_views[IO_BINDING_MAX_INPUTS];
    int profile_output_count = io_binding_export_outputs(output_views, IO_BINDING_MAX_OUTPUTS);
    int profile_input_count = io_binding_export_inputs(input_views, IO_BINDING_MAX_INPUTS);

    output_count = 0;
    input_count = 0;

    for (int i = 0; i < profile_output_count && output_count < IO_DRIVER_MAX_OUTPUTS; i++)
    {
        const io_binding_output_view_t *binding = &output_views[i];
        const device_output_profile_t *cfg = device_profile_find_output(binding->id);

        if (!cfg)
            continue;

        output_table[output_count].id = cfg->id;
        output_table[output_count].gpio = (gpio_num_t)binding->gpio;
        output_table[output_count].active_low = cfg->active_low;
        output_count++;
    }

    for (int i = 0; i < profile_input_count && input_count < IO_DRIVER_MAX_INPUTS; i++)
    {
        const io_binding_input_view_t *binding = &input_views[i];
        const device_input_profile_t *cfg = device_profile_find_input(binding->id);

        if (!cfg)
            continue;

        input_table[input_count] = (input_channel_t){
            .id = cfg->id,
            .gpio = (gpio_num_t)binding->gpio,
            .active_low = cfg->active_low,
            .debounce_samples = io_driver_clamp_debounce_samples(cfg->debounce_samples),
        };
        input_count++;
    }
}

/* ============================================================
   INIT
============================================================ */

void io_driver_init(void)
{
    int32_t state_shadow[STATE_MAX_ENTRIES] = {0};

    ESP_LOGI(TAG, "Inicializando IO Driver");
    io_driver_load_profile();
    pending_mask = 0;
    local_mask = 0;
    last_index = 0;
    last_diag_report_us = 0;

    /*
     * Boot-safe initialization:
     * - preserva o snapshot/restauração já carregado
     * - aplica bootstrap de IO no shadow local
     * - importa tudo de forma silenciosa no final
     * Evita side effects de state_set_int() durante o boot
     * (event bus / snapshot dirty / http notify / output hook).
     */
    state_export_snapshot(state_shadow, STATE_MAX_ENTRIES);

    for (size_t i = 0; i < output_count; i++)
    {
        int32_t restored_value = 0;
        int32_t boot_value = 0;
        const char *boot_origin = "startup-policy";
        gpio_config_t cfg =
        {
            .pin_bit_mask = (1ULL << output_table[i].gpio),
            .mode = GPIO_MODE_OUTPUT,
            .pull_up_en = 0,
            .pull_down_en = 0,
            .intr_type = GPIO_INTR_DISABLE
        };

        gpio_config(&cfg);

        if (!state_get_int(output_table[i].id, &restored_value))
            restored_value = 0;

        if (!failsafe_startup_value(output_table[i].id, restored_value, &boot_value, &boot_origin))
            boot_value = 0;

        gpio_set_level(output_table[i].gpio,
            io_driver_output_gpio_level(&output_table[i], boot_value != 0));

        io_driver_state_shadow_assign(state_shadow, output_table[i].id, boot_value);

        ESP_LOGI(TAG,
            "Output %u -> GPIO %d (%s, restored=%" PRId32 ", boot=%" PRId32 ", origin=%s)",
            output_table[i].id,
            output_table[i].gpio,
            output_table[i].active_low ? "active-low" : "active-high",
            restored_value,
            boot_value,
            boot_origin ? boot_origin : "startup-policy");

        local_mask |= (1U << i);
    }

    for (size_t i = 0; i < input_count; i++)
    {
        gpio_config_t cfg =
        {
            .pin_bit_mask = (1ULL << input_table[i].gpio),
            .mode = GPIO_MODE_INPUT,
            .pull_up_en = device_profile_gpio_is_input_only(input_table[i].gpio)
                ? GPIO_PULLUP_DISABLE
                : (input_table[i].active_low ? GPIO_PULLUP_ENABLE : GPIO_PULLUP_DISABLE),
            .pull_down_en = device_profile_gpio_is_input_only(input_table[i].gpio)
                ? GPIO_PULLDOWN_DISABLE
                : (input_table[i].active_low ? GPIO_PULLDOWN_DISABLE : GPIO_PULLDOWN_ENABLE),
            .intr_type = GPIO_INTR_DISABLE
        };

        gpio_config(&cfg);

        int level = gpio_get_level(input_table[i].gpio);
        uint8_t logical = input_table[i].active_low ? (level == 0) : (level != 0);

        input_table[i].last_sample = logical;
        input_table[i].stable_value = logical;
        input_table[i].stable_count = input_table[i].debounce_samples;

        io_image_set_input(input_table[i].id, logical);
        io_driver_state_shadow_assign(state_shadow, input_table[i].id, logical);

        ESP_LOGI(TAG,
            "Input %u -> GPIO %d (%s, pull-%s, debounce=%u%s)",
            input_table[i].id,
            input_table[i].gpio,
            input_table[i].active_low ? "active-low" : "active-high",
            device_profile_gpio_is_input_only(input_table[i].gpio)
                ? "externo"
                : (input_table[i].active_low ? "up" : "down"),
            input_table[i].debounce_samples,
            device_profile_gpio_is_input_only(input_table[i].gpio) ? ", input-only" : "");
    }

    state_import_snapshot(state_shadow, STATE_MAX_ENTRIES);
}

/* ============================================================
   HELPERS
============================================================ */

static bool IRAM_ATTR io_driver_is_known_state(uint16_t id)
{
    for (size_t i = 0; i < output_count; i++)
    {
        if (output_table[i].id == id)
            return true;
    }

    for (size_t i = 0; i < input_count; i++)
    {
        if (input_table[i].id == id)
            return true;
    }

    return false;
}

/* ============================================================
   STATE HOOK (🔥 NÃO EXECUTA GPIO)
============================================================ */

void IRAM_ATTR state_output_changed(uint16_t id, int32_t value)
{
    (void)value;

    for (size_t i = 0; i < output_count; i++)
    {
        if (output_table[i].id == id)
        {
            pending_mask |= (1U << i);
            break;
        }
    }

    if (io_driver_is_known_state(id))
        http_server_notify_state_change();
}

/* ============================================================
   INPUT SCAN (🔥 DETERMINÍSTICO)
============================================================ */

void IRAM_ATTR io_driver_scan_inputs(void)
{
    for (size_t i = 0; i < input_count; i++)
    {
        input_channel_t *ch = &input_table[i];
        int level = gpio_get_level(ch->gpio);
        uint8_t logical = ch->active_low ? (level == 0) : (level != 0);

        io_image_set_input(ch->id, logical);

        if (logical != ch->last_sample)
        {
            ch->last_sample = logical;
            ch->stable_count = 1;
            ch->raw_edges++;
            continue;
        }

        if (ch->stable_count < ch->debounce_samples)
        {
            ch->stable_count++;

            if (ch->stable_count < ch->debounce_samples)
                continue;
        }

        if (ch->stable_value != logical)
        {
            ch->stable_value = logical;
            ch->stable_edges++;
            state_set_int(ch->id, logical);
        }
    }
}

void io_driver_process(void)
{
    uint64_t now_us = (uint64_t)esp_timer_get_time();

    if ((now_us - last_diag_report_us) < INPUT_DIAG_PERIOD_US)
        return;

    last_diag_report_us = now_us;

    for (size_t i = 0; i < input_count; i++)
    {
        input_channel_t *ch = &input_table[i];
        uint32_t raw = ch->raw_edges;
        uint32_t stable = ch->stable_edges;
        uint32_t noise = (raw > stable) ? (raw - stable) : 0U;
        uint32_t delta_raw = raw - ch->reported_raw_edges;
        uint32_t delta_stable = stable - ch->reported_stable_edges;
        uint32_t delta_noise = (delta_raw > delta_stable) ? (delta_raw - delta_stable) : 0U;

        ch->recent_raw_edges = delta_raw;
        ch->recent_stable_edges = delta_stable;
        ch->recent_noise_edges = delta_noise;
        ch->reported_raw_edges = raw;
        ch->reported_stable_edges = stable;

        if (delta_raw == 0U && delta_stable == 0U)
            continue;

        if (delta_noise > 0U)
        {
            ESP_LOGW(TAG,
                "INPUT_DIAG: input=%u level=%u raw=%" PRIu32 " stable=%" PRIu32 " noise=%" PRIu32
                " (+raw=%" PRIu32 " +stable=%" PRIu32 " +noise=%" PRIu32 ")",
                ch->id,
                ch->stable_value,
                raw,
                stable,
                noise,
                delta_raw,
                delta_stable,
                delta_noise);
        }
        else
        {
            ESP_LOGI(TAG,
                "INPUT_DIAG: input=%u level=%u raw=%" PRIu32 " stable=%" PRIu32 " noise=%" PRIu32
                " (+raw=%" PRIu32 " +stable=%" PRIu32 ")",
                ch->id,
                ch->stable_value,
                raw,
                stable,
                noise,
                delta_raw,
                delta_stable);
        }
    }
}

int io_driver_get_input_diag(io_driver_input_diag_t *out, int max_inputs)
{
    int count = ((int)input_count < max_inputs) ? (int)input_count : max_inputs;

    if (!out || max_inputs <= 0)
        return (int)input_count;

    for (int i = 0; i < count; i++)
    {
        uint32_t raw = input_table[i].raw_edges;
        uint32_t stable = input_table[i].stable_edges;

        out[i].id = input_table[i].id;
        out[i].level = input_table[i].stable_value;
        out[i].raw_edges = raw;
        out[i].stable_edges = stable;
        out[i].noise_edges = (raw > stable) ? (raw - stable) : 0U;
        out[i].recent_raw_edges = input_table[i].recent_raw_edges;
        out[i].recent_stable_edges = input_table[i].recent_stable_edges;
        out[i].recent_noise_edges = input_table[i].recent_noise_edges;
    }

    return count;
}

/* ============================================================
   APPLY OUTPUTS (🔥 CONTROLADO)
============================================================ */

void IRAM_ATTR io_driver_update(void)
{
    uint32_t mask = pending_mask;

    if (output_count == 0U)
        return;

    for (size_t i = 0; i < output_count; i++)
    {
        uint32_t bit = (1U << i);
        bool was_local = ((local_mask & bit) != 0U);
        bool is_local = cluster_io_is_local(output_table[i].id);

        if (is_local == was_local)
            continue;

        if (is_local)
            local_mask |= bit;
        else
            local_mask &= ~bit;

        pending_mask |= bit;
        mask |= bit;
    }

    if (mask == 0)
        return;

    /* 🔥 round-robin: evita pico concentrado */
    for (size_t j = 0; j < output_count; j++)
    {
        size_t i = (last_index + j) % output_count;

        if (mask & (1U << i))
        {
            if (!cluster_io_is_local(output_table[i].id))
            {
                gpio_set_level(output_table[i].gpio,
                    io_driver_output_gpio_level(&output_table[i], false));
            }
            else
            {
                int32_t value;

                if (state_get_int(output_table[i].id, &value))
                    gpio_set_level(output_table[i].gpio,
                        io_driver_output_gpio_level(&output_table[i], value != 0));
            }

            /* 🔥 remove só este bit */
            pending_mask &= ~(1U << i);

            last_index = (uint8_t)(i + 1U);

            return; // 🔥 SÓ 1 POR CICLO
        }
    }
}

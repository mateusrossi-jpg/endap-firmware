#include "phase_load_test.h"

#include <string.h>

#include "esp_rom_sys.h"
#include "freertos/FreeRTOS.h"
#include "freertos/portmacro.h"

static volatile uint32_t phase_extra_us[PHASE_LOAD_TEST_PHASE_COUNT] = {0};
static volatile uint32_t phase_mask = 0U;
static portMUX_TYPE phase_load_test_mux = portMUX_INITIALIZER_UNLOCKED;

static inline uint32_t phase_bit(uint8_t phase)
{
    return (phase < 32U) ? (1UL << phase) : 0U;
}

void phase_load_test_set(uint8_t phase, uint32_t extra_us)
{
    uint32_t bit;

    if (phase >= PHASE_LOAD_TEST_PHASE_COUNT)
        return;

    if (extra_us > PHASE_LOAD_TEST_MAX_EXTRA_US)
        extra_us = PHASE_LOAD_TEST_MAX_EXTRA_US;

    bit = phase_bit(phase);

    portENTER_CRITICAL(&phase_load_test_mux);
    phase_extra_us[phase] = extra_us;

    if (extra_us > 0U)
        phase_mask |= bit;
    else
        phase_mask &= ~bit;

    portEXIT_CRITICAL(&phase_load_test_mux);
}

void phase_load_test_clear(void)
{
    portENTER_CRITICAL(&phase_load_test_mux);

    for (uint8_t i = 0; i < PHASE_LOAD_TEST_PHASE_COUNT; i++)
        phase_extra_us[i] = 0U;

    phase_mask = 0U;

    portEXIT_CRITICAL(&phase_load_test_mux);
}

bool phase_load_test_is_active(void)
{
    return phase_mask != 0U;
}

void phase_load_test_get(phase_load_test_snapshot_t *snapshot)
{
    uint32_t values[PHASE_LOAD_TEST_PHASE_COUNT];

    if (!snapshot)
        return;

    memset(snapshot, 0, sizeof(*snapshot));

    portENTER_CRITICAL(&phase_load_test_mux);

    for (uint8_t i = 0; i < PHASE_LOAD_TEST_PHASE_COUNT; i++)
        values[i] = phase_extra_us[i];

    portEXIT_CRITICAL(&phase_load_test_mux);

    snapshot->io_us = values[PHASE_LOAD_TEST_IO];
    snapshot->io_apply_us = values[PHASE_LOAD_TEST_IO_APPLY];
    snapshot->fieldbus_us = values[PHASE_LOAD_TEST_FIELDBUS];
    snapshot->automation_us = values[PHASE_LOAD_TEST_AUTOMATION];
    snapshot->events_us = values[PHASE_LOAD_TEST_EVENTS];
    snapshot->diagnostics_us = values[PHASE_LOAD_TEST_DIAGNOSTICS];

    for (uint8_t i = 0; i < PHASE_LOAD_TEST_PHASE_COUNT; i++)
    {
        if (values[i] > 0U)
        {
            snapshot->active = true;
            snapshot->active_phase_count++;
            snapshot->total_us += values[i];
        }
    }
}

void IRAM_ATTR phase_load_test_apply(uint8_t phase)
{
    uint32_t extra_us;

    if (phase >= PHASE_LOAD_TEST_PHASE_COUNT)
        return;

    if ((phase_mask & phase_bit(phase)) == 0U)
        return;

    extra_us = phase_extra_us[phase];
    if (extra_us == 0U)
        return;

    esp_rom_delay_us(extra_us);
}

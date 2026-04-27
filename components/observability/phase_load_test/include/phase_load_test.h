#ifndef ENDAP_PHASE_LOAD_TEST_H
#define ENDAP_PHASE_LOAD_TEST_H

#include <stdbool.h>
#include <stdint.h>

#define PHASE_LOAD_TEST_PHASE_COUNT 6U
#define PHASE_LOAD_TEST_MAX_EXTRA_US 1500U

typedef enum
{
    PHASE_LOAD_TEST_IO = 0,
    PHASE_LOAD_TEST_IO_APPLY = 1,
    PHASE_LOAD_TEST_FIELDBUS = 2,
    PHASE_LOAD_TEST_AUTOMATION = 3,
    PHASE_LOAD_TEST_EVENTS = 4,
    PHASE_LOAD_TEST_DIAGNOSTICS = 5
} phase_load_test_phase_t;

typedef struct
{
    bool active;
    uint8_t active_phase_count;
    uint32_t total_us;
    uint32_t io_us;
    uint32_t io_apply_us;
    uint32_t fieldbus_us;
    uint32_t automation_us;
    uint32_t events_us;
    uint32_t diagnostics_us;
} phase_load_test_snapshot_t;

void phase_load_test_set(uint8_t phase, uint32_t extra_us);
void phase_load_test_clear(void);
bool phase_load_test_is_active(void);
void phase_load_test_get(phase_load_test_snapshot_t *snapshot);
void phase_load_test_apply(uint8_t phase);

#endif

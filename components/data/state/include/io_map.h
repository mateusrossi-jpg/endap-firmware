#pragma once

#include <stdint.h>
#include <stdbool.h>

#define ENDAP_INPUT_BASE_ID            10U
#define ENDAP_OUTPUT_BASE_ID           100U

#define ENDAP_MAX_INPUT_SLOTS          16U
#define ENDAP_MAX_OUTPUT_SLOTS         16U

#define ENDAP_DEFAULT_ACTIVE_INPUTS    2U
#define ENDAP_DEFAULT_ACTIVE_OUTPUTS   3U

#define ENDAP_INPUT_ID(slot)   ((uint16_t)(ENDAP_INPUT_BASE_ID + (uint16_t)(slot)))
#define ENDAP_OUTPUT_ID(slot)  ((uint16_t)(ENDAP_OUTPUT_BASE_ID + (uint16_t)(slot)))

#define ENDAP_INPUT_SLOT_FROM_ID(id)   ((int)((uint16_t)(id) - ENDAP_INPUT_BASE_ID))
#define ENDAP_OUTPUT_SLOT_FROM_ID(id)  ((int)((uint16_t)(id) - ENDAP_OUTPUT_BASE_ID))

#define ENDAP_INPUT_ID_MIN             ENDAP_INPUT_ID(0)
#define ENDAP_INPUT_ID_MAX             ENDAP_INPUT_ID(ENDAP_MAX_INPUT_SLOTS - 1U)
#define ENDAP_OUTPUT_ID_MIN            ENDAP_OUTPUT_ID(0)
#define ENDAP_OUTPUT_ID_MAX            ENDAP_OUTPUT_ID(ENDAP_MAX_OUTPUT_SLOTS - 1U)

#define ENDAP_INPUT_0_ID               ENDAP_INPUT_ID(0)
#define ENDAP_INPUT_1_ID               ENDAP_INPUT_ID(1)
#define ENDAP_INPUT_2_ID               ENDAP_INPUT_ID(2)
#define ENDAP_INPUT_3_ID               ENDAP_INPUT_ID(3)
#define ENDAP_INPUT_4_ID               ENDAP_INPUT_ID(4)
#define ENDAP_INPUT_5_ID               ENDAP_INPUT_ID(5)
#define ENDAP_INPUT_6_ID               ENDAP_INPUT_ID(6)
#define ENDAP_INPUT_7_ID               ENDAP_INPUT_ID(7)
#define ENDAP_INPUT_8_ID               ENDAP_INPUT_ID(8)
#define ENDAP_INPUT_9_ID               ENDAP_INPUT_ID(9)
#define ENDAP_INPUT_10_ID              ENDAP_INPUT_ID(10)
#define ENDAP_INPUT_11_ID              ENDAP_INPUT_ID(11)
#define ENDAP_INPUT_12_ID              ENDAP_INPUT_ID(12)
#define ENDAP_INPUT_13_ID              ENDAP_INPUT_ID(13)
#define ENDAP_INPUT_14_ID              ENDAP_INPUT_ID(14)
#define ENDAP_INPUT_15_ID              ENDAP_INPUT_ID(15)

#define ENDAP_OUTPUT_0_ID              ENDAP_OUTPUT_ID(0)
#define ENDAP_OUTPUT_1_ID              ENDAP_OUTPUT_ID(1)
#define ENDAP_OUTPUT_2_ID              ENDAP_OUTPUT_ID(2)
#define ENDAP_OUTPUT_3_ID              ENDAP_OUTPUT_ID(3)
#define ENDAP_OUTPUT_4_ID              ENDAP_OUTPUT_ID(4)
#define ENDAP_OUTPUT_5_ID              ENDAP_OUTPUT_ID(5)
#define ENDAP_OUTPUT_6_ID              ENDAP_OUTPUT_ID(6)
#define ENDAP_OUTPUT_7_ID              ENDAP_OUTPUT_ID(7)
#define ENDAP_OUTPUT_8_ID              ENDAP_OUTPUT_ID(8)
#define ENDAP_OUTPUT_9_ID              ENDAP_OUTPUT_ID(9)
#define ENDAP_OUTPUT_10_ID             ENDAP_OUTPUT_ID(10)
#define ENDAP_OUTPUT_11_ID             ENDAP_OUTPUT_ID(11)
#define ENDAP_OUTPUT_12_ID             ENDAP_OUTPUT_ID(12)
#define ENDAP_OUTPUT_13_ID             ENDAP_OUTPUT_ID(13)
#define ENDAP_OUTPUT_14_ID             ENDAP_OUTPUT_ID(14)
#define ENDAP_OUTPUT_15_ID             ENDAP_OUTPUT_ID(15)

static inline bool endap_is_valid_input_slot(int slot)
{
    return slot >= 0 && slot < (int)ENDAP_MAX_INPUT_SLOTS;
}

static inline bool endap_is_valid_output_slot(int slot)
{
    return slot >= 0 && slot < (int)ENDAP_MAX_OUTPUT_SLOTS;
}

static inline bool endap_is_valid_input_id(uint16_t id)
{
    return id >= ENDAP_INPUT_ID_MIN && id <= ENDAP_INPUT_ID_MAX;
}

static inline bool endap_is_valid_output_id(uint16_t id)
{
    return id >= ENDAP_OUTPUT_ID_MIN && id <= ENDAP_OUTPUT_ID_MAX;
}

static inline bool endap_is_default_input_id(uint16_t id)
{
    return endap_is_valid_input_id(id) &&
           ENDAP_INPUT_SLOT_FROM_ID(id) < (int)ENDAP_DEFAULT_ACTIVE_INPUTS;
}

static inline bool endap_is_default_output_id(uint16_t id)
{
    return endap_is_valid_output_id(id) &&
           ENDAP_OUTPUT_SLOT_FROM_ID(id) < (int)ENDAP_DEFAULT_ACTIVE_OUTPUTS;
}

static inline bool endap_is_extra_input_id(uint16_t id)
{
    return endap_is_valid_input_id(id) && !endap_is_default_input_id(id);
}

static inline bool endap_is_extra_output_id(uint16_t id)
{
    return endap_is_valid_output_id(id) && !endap_is_default_output_id(id);
}

#pragma once

#include "automation_node.h"

#include <stdbool.h>
#include <stdint.h>

#define AUTOMATION_ENGINE_MAX_NODES 16

typedef enum
{
    AUTOMATION_OP_GT = 0,
    AUTOMATION_OP_GE,
    AUTOMATION_OP_EQ,
    AUTOMATION_OP_NE,
    AUTOMATION_OP_LE,
    AUTOMATION_OP_LT,
    AUTOMATION_OP_MAX
} automation_operator_t;

typedef enum
{
    AUTOMATION_MODE_FOLLOW = 0,
    AUTOMATION_MODE_PULSE_MS,
    AUTOMATION_MODE_ON_DELAY_MS,
    AUTOMATION_MODE_OFF_DELAY_MS,
    AUTOMATION_MODE_TOGGLE,
    AUTOMATION_MODE_FORCE_ON,
    AUTOMATION_MODE_FORCE_OFF,
    AUTOMATION_MODE_MAX
} automation_mode_t;

typedef enum
{
    AUTOMATION_RESULT_OK = 0,
    AUTOMATION_RESULT_DUPLICATE,
    AUTOMATION_RESULT_FULL,
    AUTOMATION_RESULT_INVALID,
    AUTOMATION_RESULT_CONFLICT
} automation_result_t;

typedef enum
{
    AUTOMATION_ACTION_IDLE = 0,
    AUTOMATION_ACTION_APPLIED_LOCAL,
    AUTOMATION_ACTION_DISPATCHED_REMOTE,
    AUTOMATION_ACTION_DISPATCH_FAILED,
    AUTOMATION_ACTION_REMOTE_OWNER_OFFLINE,
    AUTOMATION_ACTION_TARGET_UNRESOLVED,
    AUTOMATION_ACTION_BLOCKED_FAILSAFE
} automation_action_result_t;

typedef struct
{
    uint32_t last_trigger_ms;
    uint32_t last_eval_ms;
    uint32_t last_action_ms;
    uint8_t last_condition;
    uint8_t last_action_value;
    uint8_t last_action_result;
    uint8_t last_target_local;
    uint32_t last_target_owner;
    uint32_t last_target_original_owner;
} automation_rule_diag_t;

void automation_engine_init(void);
bool automation_engine_add_node(int input, int threshold, int output);
automation_result_t automation_engine_add_rule(
    int input,
    uint8_t op,
    uint8_t mode,
    int threshold,
    uint16_t duration_ms,
    int output,
    int on_true,
    int on_false);
bool automation_engine_remove_node_at(int index);
void automation_engine_clear(void);
int automation_engine_get_node_count(void);
int automation_engine_export_nodes(automation_node_t *out, int max_nodes);
int automation_engine_export_diags(automation_rule_diag_t *out, int max_nodes);
bool automation_engine_has_persisted_config(void);
bool automation_engine_operator_from_code(const char *text, uint8_t *out_operator);
const char *automation_engine_operator_to_code(uint8_t op);
const char *automation_engine_operator_to_symbol(uint8_t op);
bool automation_engine_mode_from_code(const char *text, uint8_t *out_mode);
const char *automation_engine_mode_to_code(uint8_t mode);
const char *automation_engine_mode_to_label(uint8_t mode);
const char *automation_engine_action_result_name(uint8_t result);
void automation_engine_tick_1ms(void);

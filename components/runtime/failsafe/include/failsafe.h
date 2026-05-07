#pragma once

#include <stdbool.h>
#include <stdint.h>

#define FAILSAFE_MAX_OUTPUTS 16
#define FAILSAFE_REASON_LEN 24

typedef enum
{
    FAILSAFE_ACTION_FORCE_OFF = 0,
    FAILSAFE_ACTION_FORCE_ON,
    FAILSAFE_ACTION_HOLD_LAST,
    FAILSAFE_ACTION_RESTORE_SNAPSHOT,
    FAILSAFE_ACTION_SAFE_VALUE,
    FAILSAFE_ACTION_MAX
} failsafe_action_t;

typedef enum
{
    FAILSAFE_RECOVERY_AUTO = 0,
    FAILSAFE_RECOVERY_MANUAL,
    FAILSAFE_RECOVERY_MAX
} failsafe_recovery_t;

typedef enum
{
    FAILSAFE_REASON_NONE = 0,
    FAILSAFE_REASON_BOOT,
    FAILSAFE_REASON_COMM_LOSS,
    FAILSAFE_REASON_RUNTIME_FAULT,
    FAILSAFE_REASON_WATCHDOG,
    FAILSAFE_REASON_NODE_OFFLINE,
    FAILSAFE_REASON_NODE_REVOKED,
    FAILSAFE_REASON_UNAUTHORIZED,
    FAILSAFE_REASON_CONFIG_INVALID,
    FAILSAFE_REASON_MANUAL_TEST,
    FAILSAFE_REASON_MAX
} failsafe_reason_t;

/* Compatibilidade com a primeira rodada do fail-safe. */
typedef failsafe_action_t failsafe_mode_t;
#define FAILSAFE_MODE_FORCE_OFF        FAILSAFE_ACTION_FORCE_OFF
#define FAILSAFE_MODE_FORCE_ON         FAILSAFE_ACTION_FORCE_ON
#define FAILSAFE_MODE_HOLD_LAST        FAILSAFE_ACTION_HOLD_LAST
#define FAILSAFE_MODE_RESTORE_SNAPSHOT FAILSAFE_ACTION_RESTORE_SNAPSHOT
#define FAILSAFE_MODE_SAFE_VALUE       FAILSAFE_ACTION_SAFE_VALUE
#define FAILSAFE_MODE_MAX              FAILSAFE_ACTION_MAX

typedef enum
{
    FAILSAFE_COMMAND_MANUAL = 0,
    FAILSAFE_COMMAND_AUTOMATION,
    FAILSAFE_COMMAND_REMOTE,
    FAILSAFE_COMMAND_RECOVERY,
} failsafe_command_source_t;

typedef struct
{
    uint16_t output_id;
    bool enabled;
    failsafe_action_t boot_action;
    failsafe_action_t comm_loss_action;
    failsafe_action_t runtime_fault_action;
    int32_t safe_value;
    failsafe_recovery_t recovery_mode;
    bool manual_reset_required;
    bool failsafe_active;
    failsafe_reason_t last_reason;
    int32_t last_applied_value;

    /* Campos mantidos para consumidores atuais da API/status. */
    failsafe_mode_t startup_mode;
    failsafe_mode_t comm_loss_mode;
    bool manual_rearm;
    bool active;
    int32_t applied_value;
    char reason[FAILSAFE_REASON_LEN];
} failsafe_output_status_t;

void failsafe_init(void);
bool failsafe_load(void);
bool failsafe_save(void);

bool failsafe_action_from_code(const char *text, failsafe_action_t *out_action);
const char *failsafe_action_to_code(failsafe_action_t action);
const char *failsafe_action_to_label(failsafe_action_t action);
bool failsafe_mode_from_code(const char *text, failsafe_mode_t *out_mode);
const char *failsafe_mode_to_code(failsafe_mode_t mode);
const char *failsafe_mode_to_label(failsafe_mode_t mode);
bool failsafe_recovery_from_code(const char *text, failsafe_recovery_t *out_recovery);
const char *failsafe_recovery_to_code(failsafe_recovery_t recovery);
const char *failsafe_recovery_to_label(failsafe_recovery_t recovery);
const char *failsafe_reason_name(failsafe_reason_t reason);
const char *failsafe_reason_label(failsafe_reason_t reason);

bool failsafe_set_output_policy(uint16_t output_id,
                                bool enabled,
                                failsafe_action_t boot_action,
                                failsafe_action_t comm_loss_action,
                                failsafe_action_t runtime_fault_action,
                                int32_t safe_value,
                                failsafe_recovery_t recovery_mode);

bool failsafe_get_output_policy(uint16_t output_id, failsafe_output_status_t *out);

bool failsafe_set_policy(uint16_t output_id,
                         failsafe_mode_t startup_mode,
                         failsafe_mode_t comm_loss_mode,
                         int32_t safe_value,
                         bool manual_rearm);

bool failsafe_get_policy(uint16_t output_id, failsafe_output_status_t *out);
int failsafe_export(failsafe_output_status_t *out, int max_outputs);

bool failsafe_startup_value(uint16_t output_id,
                            int32_t restored_value,
                            int32_t *out_value,
                            const char **out_origin);

bool failsafe_guard_command(uint16_t output_id,
                            int32_t requested_value,
                            failsafe_command_source_t source,
                            int32_t *out_effective_value,
                            const char **out_reason);

bool failsafe_apply_for_reason(uint16_t output_id,
                               failsafe_reason_t reason,
                               int32_t *out_safe_value);
int failsafe_apply_for_reason_all(failsafe_reason_t reason);
bool failsafe_clear_manual_reset(uint16_t output_id);
bool failsafe_is_active(uint16_t output_id);
bool failsafe_trigger_comm_loss(uint16_t output_id, int32_t *out_safe_value);
bool failsafe_trigger_runtime_fault(uint16_t output_id, int32_t *out_safe_value);
bool failsafe_trigger_manual_test(uint16_t output_id, int32_t *out_safe_value);
bool failsafe_rearm(uint16_t output_id);

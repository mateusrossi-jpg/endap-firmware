#include "failsafe.h"

#include "device_profile.h"
#include "state.h"

#include "esp_log.h"
#include "esp_system.h"
#include "nvs.h"

#include <inttypes.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#define TAG "FAILSAFE"
#define FAILSAFE_NAMESPACE "failsafe"
#define FAILSAFE_KEY "policies"
#define FAILSAFE_MAGIC 0x46534146U
#define FAILSAFE_VERSION 2U
#define FAILSAFE_VERSION_V1 1U

typedef struct
{
    uint16_t output_id;
    uint8_t enabled;
    uint8_t boot_action;
    uint8_t comm_loss_action;
    uint8_t runtime_fault_action;
    uint8_t recovery_mode;
    uint8_t reserved;
    int32_t safe_value;
} failsafe_policy_storage_t;

typedef struct
{
    uint32_t magic;
    uint16_t version;
    uint16_t count;
    failsafe_policy_storage_t policies[FAILSAFE_MAX_OUTPUTS];
    uint32_t crc;
} failsafe_blob_t;

typedef struct
{
    uint16_t output_id;
    uint8_t startup_mode;
    uint8_t comm_loss_mode;
    uint8_t manual_rearm;
    uint8_t reserved;
    int32_t safe_value;
} failsafe_policy_storage_v1_t;

typedef struct
{
    uint32_t magic;
    uint16_t version;
    uint16_t count;
    failsafe_policy_storage_v1_t policies[FAILSAFE_MAX_OUTPUTS];
    uint32_t crc;
} failsafe_blob_v1_t;

typedef struct
{
    failsafe_policy_storage_t policy;
    uint8_t active;
    failsafe_reason_t last_reason;
    int32_t last_applied_value;
} failsafe_runtime_entry_t;

static failsafe_runtime_entry_t entries[FAILSAFE_MAX_OUTPUTS];
static uint16_t entry_count = 0U;
static failsafe_reason_t boot_reason = FAILSAFE_REASON_BOOT;

static uint32_t failsafe_crc32(const uint8_t *data, size_t len)
{
    uint32_t crc = 2166136261u;

    for (size_t i = 0; i < len; i++)
    {
        crc ^= data[i];
        crc *= 16777619u;
    }

    return crc;
}

static bool failsafe_action_valid(uint8_t action)
{
    return action < FAILSAFE_ACTION_MAX;
}

static bool failsafe_recovery_valid(uint8_t recovery)
{
    return recovery < FAILSAFE_RECOVERY_MAX;
}

static int32_t failsafe_binary_value(int32_t value)
{
    return value ? 1 : 0;
}

static int32_t failsafe_apply_action(failsafe_action_t action,
                                     int32_t current_value,
                                     int32_t snapshot_value,
                                     int32_t safe_value)
{
    switch (action)
    {
        case FAILSAFE_ACTION_FORCE_ON:
            return 1;
        case FAILSAFE_ACTION_HOLD_LAST:
            return failsafe_binary_value(current_value);
        case FAILSAFE_ACTION_RESTORE_SNAPSHOT:
            return failsafe_binary_value(snapshot_value);
        case FAILSAFE_ACTION_SAFE_VALUE:
            return failsafe_binary_value(safe_value);
        case FAILSAFE_ACTION_FORCE_OFF:
        default:
            return 0;
    }
}

static failsafe_action_t failsafe_action_for_reason(const failsafe_runtime_entry_t *entry,
                                                    failsafe_reason_t reason)
{
    if (!entry)
        return FAILSAFE_ACTION_FORCE_OFF;

    switch (reason)
    {
        case FAILSAFE_REASON_BOOT:
            return (failsafe_action_t)entry->policy.boot_action;

        case FAILSAFE_REASON_COMM_LOSS:
        case FAILSAFE_REASON_NODE_OFFLINE:
        case FAILSAFE_REASON_NODE_REVOKED:
        case FAILSAFE_REASON_UNAUTHORIZED:
            return (failsafe_action_t)entry->policy.comm_loss_action;

        case FAILSAFE_REASON_RUNTIME_FAULT:
        case FAILSAFE_REASON_WATCHDOG:
        case FAILSAFE_REASON_CONFIG_INVALID:
        case FAILSAFE_REASON_MANUAL_TEST:
            return (failsafe_action_t)entry->policy.runtime_fault_action;

        case FAILSAFE_REASON_NONE:
        default:
            return FAILSAFE_ACTION_HOLD_LAST;
    }
}

static failsafe_runtime_entry_t *failsafe_find(uint16_t output_id)
{
    for (uint16_t i = 0U; i < entry_count; i++)
    {
        if (entries[i].policy.output_id == output_id)
            return &entries[i];
    }

    return NULL;
}

static const failsafe_runtime_entry_t *failsafe_find_const(uint16_t output_id)
{
    return failsafe_find(output_id);
}

static void failsafe_default_entry(failsafe_runtime_entry_t *entry, uint16_t output_id)
{
    if (!entry)
        return;

    memset(entry, 0, sizeof(*entry));
    entry->policy.output_id = output_id;
    entry->policy.enabled = 1U;
    entry->policy.boot_action = (uint8_t)FAILSAFE_ACTION_FORCE_OFF;
    entry->policy.comm_loss_action = (uint8_t)FAILSAFE_ACTION_FORCE_OFF;
    entry->policy.runtime_fault_action = (uint8_t)FAILSAFE_ACTION_FORCE_OFF;
    entry->policy.recovery_mode = (uint8_t)FAILSAFE_RECOVERY_AUTO;
    entry->policy.safe_value = 0;
    entry->active = 0U;
    entry->last_reason = FAILSAFE_REASON_NONE;
    entry->last_applied_value = 0;
}

static failsafe_reason_t failsafe_detect_boot_reason(void)
{
    switch (esp_reset_reason())
    {
        case ESP_RST_TASK_WDT:
        case ESP_RST_INT_WDT:
        case ESP_RST_WDT:
            return FAILSAFE_REASON_WATCHDOG;

        case ESP_RST_PANIC:
            return FAILSAFE_REASON_RUNTIME_FAULT;

        default:
            return FAILSAFE_REASON_BOOT;
    }
}

static bool failsafe_policy_equals(const failsafe_policy_storage_t *a,
                                   const failsafe_policy_storage_t *b)
{
    return a && b && memcmp(a, b, sizeof(*a)) == 0;
}

static void failsafe_fill_status(const failsafe_runtime_entry_t *entry,
                                 failsafe_output_status_t *out)
{
    failsafe_action_t boot_action;
    failsafe_action_t comm_loss_action;
    failsafe_action_t runtime_fault_action;
    failsafe_recovery_t recovery_mode;

    if (!entry || !out)
        return;

    memset(out, 0, sizeof(*out));

    boot_action = (failsafe_action_t)entry->policy.boot_action;
    comm_loss_action = (failsafe_action_t)entry->policy.comm_loss_action;
    runtime_fault_action = (failsafe_action_t)entry->policy.runtime_fault_action;
    recovery_mode = (failsafe_recovery_t)entry->policy.recovery_mode;

    out->output_id = entry->policy.output_id;
    out->enabled = entry->policy.enabled != 0U;
    out->boot_action = boot_action;
    out->comm_loss_action = comm_loss_action;
    out->runtime_fault_action = runtime_fault_action;
    out->safe_value = failsafe_binary_value(entry->policy.safe_value);
    out->recovery_mode = recovery_mode;
    out->manual_reset_required = recovery_mode == FAILSAFE_RECOVERY_MANUAL;
    out->failsafe_active = entry->active != 0U;
    out->last_reason = entry->last_reason;
    out->last_applied_value = failsafe_binary_value(entry->last_applied_value);

    out->startup_mode = boot_action;
    out->comm_loss_mode = comm_loss_action;
    out->manual_rearm = out->manual_reset_required;
    out->active = out->failsafe_active;
    out->applied_value = out->last_applied_value;
    snprintf(out->reason, sizeof(out->reason), "%s", failsafe_reason_name(entry->last_reason));
}

static bool failsafe_apply_loaded_policy(const failsafe_policy_storage_t *policy)
{
    failsafe_runtime_entry_t *entry;

    if (!policy || !device_profile_is_valid_output(policy->output_id))
        return false;

    if (!failsafe_action_valid(policy->boot_action) ||
        !failsafe_action_valid(policy->comm_loss_action) ||
        !failsafe_action_valid(policy->runtime_fault_action) ||
        !failsafe_recovery_valid(policy->recovery_mode))
    {
        return false;
    }

    entry = failsafe_find(policy->output_id);
    if (!entry)
        return false;

    entry->policy = *policy;
    entry->policy.enabled = policy->enabled ? 1U : 0U;
    entry->policy.safe_value = failsafe_binary_value(policy->safe_value);
    entry->active = 0U;
    entry->last_reason = FAILSAFE_REASON_NONE;
    entry->last_applied_value = 0;
    return true;
}

static bool failsafe_v1_mode_to_action(uint8_t mode, failsafe_action_t *out_action)
{
    if (!out_action)
        return false;

    switch (mode)
    {
        case 0:
            *out_action = FAILSAFE_ACTION_HOLD_LAST;
            return true;
        case 1:
            *out_action = FAILSAFE_ACTION_FORCE_OFF;
            return true;
        case 2:
            *out_action = FAILSAFE_ACTION_FORCE_ON;
            return true;
        case 3:
            *out_action = FAILSAFE_ACTION_SAFE_VALUE;
            return true;
        default:
            return false;
    }
}

static bool failsafe_apply_loaded_policy_v1(const failsafe_policy_storage_v1_t *policy)
{
    failsafe_policy_storage_t next = {0};
    failsafe_action_t boot_action;
    failsafe_action_t comm_loss_action;

    if (!policy ||
        !failsafe_v1_mode_to_action(policy->startup_mode, &boot_action) ||
        !failsafe_v1_mode_to_action(policy->comm_loss_mode, &comm_loss_action))
    {
        return false;
    }

    next.output_id = policy->output_id;
    next.enabled = 1U;
    next.boot_action = (uint8_t)boot_action;
    next.comm_loss_action = (uint8_t)comm_loss_action;
    next.runtime_fault_action = (uint8_t)FAILSAFE_ACTION_FORCE_OFF;
    next.recovery_mode = policy->manual_rearm ? (uint8_t)FAILSAFE_RECOVERY_MANUAL : (uint8_t)FAILSAFE_RECOVERY_AUTO;
    next.safe_value = failsafe_binary_value(policy->safe_value);

    return failsafe_apply_loaded_policy(&next);
}

bool failsafe_save(void)
{
    nvs_handle_t nvs;
    failsafe_blob_t blob = {0};

    blob.magic = FAILSAFE_MAGIC;
    blob.version = FAILSAFE_VERSION;
    blob.count = entry_count;

    for (uint16_t i = 0U; i < entry_count; i++)
        blob.policies[i] = entries[i].policy;

    blob.crc = failsafe_crc32((const uint8_t *)&blob, sizeof(blob) - sizeof(blob.crc));

    if (nvs_open(FAILSAFE_NAMESPACE, NVS_READWRITE, &nvs) != ESP_OK)
        return false;

    if (nvs_set_blob(nvs, FAILSAFE_KEY, &blob, sizeof(blob)) != ESP_OK ||
        nvs_commit(nvs) != ESP_OK)
    {
        nvs_close(nvs);
        return false;
    }

    nvs_close(nvs);
    return true;
}

bool failsafe_load(void)
{
    nvs_handle_t nvs;
    size_t len = 0U;
    bool loaded = false;

    if (nvs_open(FAILSAFE_NAMESPACE, NVS_READONLY, &nvs) != ESP_OK)
        return false;

    if (nvs_get_blob(nvs, FAILSAFE_KEY, NULL, &len) != ESP_OK)
    {
        nvs_close(nvs);
        return false;
    }

    if (len == sizeof(failsafe_blob_t))
    {
        failsafe_blob_t blob = {0};
        uint32_t crc;

        if (nvs_get_blob(nvs, FAILSAFE_KEY, &blob, &len) == ESP_OK &&
            blob.magic == FAILSAFE_MAGIC &&
            blob.version == FAILSAFE_VERSION &&
            blob.count <= FAILSAFE_MAX_OUTPUTS)
        {
            crc = failsafe_crc32((const uint8_t *)&blob, sizeof(blob) - sizeof(blob.crc));
            if (crc == blob.crc)
            {
                for (uint16_t i = 0U; i < blob.count; i++)
                    loaded = failsafe_apply_loaded_policy(&blob.policies[i]) || loaded;
            }
        }
    }
    else if (len == sizeof(failsafe_blob_v1_t))
    {
        failsafe_blob_v1_t blob = {0};
        uint32_t crc;

        if (nvs_get_blob(nvs, FAILSAFE_KEY, &blob, &len) == ESP_OK &&
            blob.magic == FAILSAFE_MAGIC &&
            blob.version == FAILSAFE_VERSION_V1 &&
            blob.count <= FAILSAFE_MAX_OUTPUTS)
        {
            crc = failsafe_crc32((const uint8_t *)&blob, sizeof(blob) - sizeof(blob.crc));
            if (crc == blob.crc)
            {
                for (uint16_t i = 0U; i < blob.count; i++)
                    loaded = failsafe_apply_loaded_policy_v1(&blob.policies[i]) || loaded;
            }
        }
    }

    nvs_close(nvs);
    return loaded;
}

void failsafe_init(void)
{
    bool loaded;

    entry_count = 0U;
    memset(entries, 0, sizeof(entries));
    boot_reason = failsafe_detect_boot_reason();

    for (int i = 0; i < device_profile_output_count() && entry_count < FAILSAFE_MAX_OUTPUTS; i++)
    {
        uint16_t output_id = device_profile_output_id_at(i);
        if (output_id == 0U)
            continue;

        failsafe_default_entry(&entries[entry_count], output_id);
        entry_count++;
    }

    loaded = failsafe_load();
    if (!loaded)
        (void)failsafe_save();
}

bool failsafe_action_from_code(const char *text, failsafe_action_t *out_action)
{
    if (!text || !out_action)
        return false;

    if (strcmp(text, "force_off") == 0 || strcmp(text, "off") == 0)
        *out_action = FAILSAFE_ACTION_FORCE_OFF;
    else if (strcmp(text, "force_on") == 0 || strcmp(text, "on") == 0)
        *out_action = FAILSAFE_ACTION_FORCE_ON;
    else if (strcmp(text, "hold") == 0 || strcmp(text, "hold_last") == 0)
        *out_action = FAILSAFE_ACTION_HOLD_LAST;
    else if (strcmp(text, "restore_snapshot") == 0 || strcmp(text, "restore") == 0 || strcmp(text, "snapshot") == 0)
        *out_action = FAILSAFE_ACTION_RESTORE_SNAPSHOT;
    else if (strcmp(text, "safe_value") == 0 || strcmp(text, "safe") == 0)
        *out_action = FAILSAFE_ACTION_SAFE_VALUE;
    else
        return false;

    return true;
}

const char *failsafe_action_to_code(failsafe_action_t action)
{
    switch (action)
    {
        case FAILSAFE_ACTION_FORCE_ON:
            return "force_on";
        case FAILSAFE_ACTION_HOLD_LAST:
            return "hold_last";
        case FAILSAFE_ACTION_RESTORE_SNAPSHOT:
            return "restore_snapshot";
        case FAILSAFE_ACTION_SAFE_VALUE:
            return "safe_value";
        case FAILSAFE_ACTION_FORCE_OFF:
        default:
            return "force_off";
    }
}

const char *failsafe_action_to_label(failsafe_action_t action)
{
    switch (action)
    {
        case FAILSAFE_ACTION_FORCE_ON:
            return "forcar ON";
        case FAILSAFE_ACTION_HOLD_LAST:
            return "manter ultimo estado";
        case FAILSAFE_ACTION_RESTORE_SNAPSHOT:
            return "restaurar snapshot";
        case FAILSAFE_ACTION_SAFE_VALUE:
            return "valor seguro";
        case FAILSAFE_ACTION_FORCE_OFF:
        default:
            return "forcar OFF";
    }
}

bool failsafe_mode_from_code(const char *text, failsafe_mode_t *out_mode)
{
    return failsafe_action_from_code(text, out_mode);
}

const char *failsafe_mode_to_code(failsafe_mode_t mode)
{
    return failsafe_action_to_code(mode);
}

const char *failsafe_mode_to_label(failsafe_mode_t mode)
{
    return failsafe_action_to_label(mode);
}

bool failsafe_recovery_from_code(const char *text, failsafe_recovery_t *out_recovery)
{
    if (!text || !out_recovery)
        return false;

    if (strcmp(text, "auto") == 0 || strcmp(text, "automatic") == 0)
        *out_recovery = FAILSAFE_RECOVERY_AUTO;
    else if (strcmp(text, "manual") == 0 || strcmp(text, "manual_rearm") == 0 || strcmp(text, "manual_reset") == 0)
        *out_recovery = FAILSAFE_RECOVERY_MANUAL;
    else
        return false;

    return true;
}

const char *failsafe_recovery_to_code(failsafe_recovery_t recovery)
{
    switch (recovery)
    {
        case FAILSAFE_RECOVERY_MANUAL:
            return "manual";
        case FAILSAFE_RECOVERY_AUTO:
        default:
            return "auto";
    }
}

const char *failsafe_recovery_to_label(failsafe_recovery_t recovery)
{
    switch (recovery)
    {
        case FAILSAFE_RECOVERY_MANUAL:
            return "manual";
        case FAILSAFE_RECOVERY_AUTO:
        default:
            return "automatica";
    }
}

const char *failsafe_reason_name(failsafe_reason_t reason)
{
    switch (reason)
    {
        case FAILSAFE_REASON_BOOT:
            return "boot";
        case FAILSAFE_REASON_COMM_LOSS:
            return "comm_loss";
        case FAILSAFE_REASON_RUNTIME_FAULT:
            return "runtime_fault";
        case FAILSAFE_REASON_WATCHDOG:
            return "watchdog";
        case FAILSAFE_REASON_NODE_OFFLINE:
            return "node_offline";
        case FAILSAFE_REASON_NODE_REVOKED:
            return "node_revoked";
        case FAILSAFE_REASON_UNAUTHORIZED:
            return "unauthorized";
        case FAILSAFE_REASON_CONFIG_INVALID:
            return "config_invalid";
        case FAILSAFE_REASON_MANUAL_TEST:
            return "manual_test";
        case FAILSAFE_REASON_NONE:
        default:
            return "none";
    }
}

const char *failsafe_reason_label(failsafe_reason_t reason)
{
    switch (reason)
    {
        case FAILSAFE_REASON_BOOT:
            return "boot/reboot";
        case FAILSAFE_REASON_COMM_LOSS:
            return "perda de comunicacao";
        case FAILSAFE_REASON_RUNTIME_FAULT:
            return "falha de runtime";
        case FAILSAFE_REASON_WATCHDOG:
            return "watchdog/reset";
        case FAILSAFE_REASON_NODE_OFFLINE:
            return "no offline";
        case FAILSAFE_REASON_NODE_REVOKED:
            return "no revogado";
        case FAILSAFE_REASON_UNAUTHORIZED:
            return "comando nao autorizado";
        case FAILSAFE_REASON_CONFIG_INVALID:
            return "configuracao invalida";
        case FAILSAFE_REASON_MANUAL_TEST:
            return "teste manual";
        case FAILSAFE_REASON_NONE:
        default:
            return "nenhum";
    }
}

bool failsafe_set_output_policy(uint16_t output_id,
                                bool enabled,
                                failsafe_action_t boot_action,
                                failsafe_action_t comm_loss_action,
                                failsafe_action_t runtime_fault_action,
                                int32_t safe_value,
                                failsafe_recovery_t recovery_mode)
{
    failsafe_runtime_entry_t *entry = failsafe_find(output_id);
    failsafe_policy_storage_t next;

    if (!entry ||
        !failsafe_action_valid((uint8_t)boot_action) ||
        !failsafe_action_valid((uint8_t)comm_loss_action) ||
        !failsafe_action_valid((uint8_t)runtime_fault_action) ||
        !failsafe_recovery_valid((uint8_t)recovery_mode))
    {
        return false;
    }

    next = entry->policy;
    next.enabled = enabled ? 1U : 0U;
    next.boot_action = (uint8_t)boot_action;
    next.comm_loss_action = (uint8_t)comm_loss_action;
    next.runtime_fault_action = (uint8_t)runtime_fault_action;
    next.safe_value = failsafe_binary_value(safe_value);
    next.recovery_mode = (uint8_t)recovery_mode;

    if (failsafe_policy_equals(&entry->policy, &next))
        return true;

    entry->policy = next;
    if (!enabled)
    {
        entry->active = 0U;
        entry->last_reason = FAILSAFE_REASON_NONE;
    }

    return failsafe_save();
}

bool failsafe_get_output_policy(uint16_t output_id, failsafe_output_status_t *out)
{
    const failsafe_runtime_entry_t *entry = failsafe_find_const(output_id);

    if (!entry || !out)
        return false;

    failsafe_fill_status(entry, out);
    return true;
}

bool failsafe_set_policy(uint16_t output_id,
                         failsafe_mode_t startup_mode,
                         failsafe_mode_t comm_loss_mode,
                         int32_t safe_value,
                         bool manual_rearm)
{
    failsafe_runtime_entry_t *entry = failsafe_find(output_id);
    failsafe_action_t runtime_fault_action = FAILSAFE_ACTION_FORCE_OFF;

    if (entry)
        runtime_fault_action = (failsafe_action_t)entry->policy.runtime_fault_action;

    return failsafe_set_output_policy(output_id,
                                      true,
                                      startup_mode,
                                      comm_loss_mode,
                                      runtime_fault_action,
                                      safe_value,
                                      manual_rearm ? FAILSAFE_RECOVERY_MANUAL : FAILSAFE_RECOVERY_AUTO);
}

bool failsafe_get_policy(uint16_t output_id, failsafe_output_status_t *out)
{
    return failsafe_get_output_policy(output_id, out);
}

int failsafe_export(failsafe_output_status_t *out, int max_outputs)
{
    int copy_count = ((int)entry_count < max_outputs) ? (int)entry_count : max_outputs;

    if (!out || max_outputs <= 0)
        return (int)entry_count;

    for (int i = 0; i < copy_count; i++)
        failsafe_fill_status(&entries[i], &out[i]);

    return copy_count;
}

bool failsafe_startup_value(uint16_t output_id,
                            int32_t restored_value,
                            int32_t *out_value,
                            const char **out_origin)
{
    failsafe_runtime_entry_t *entry = failsafe_find(output_id);
    failsafe_action_t action;
    int32_t value;

    if (!entry || !out_value)
        return false;

    if (!entry->policy.enabled)
    {
        *out_value = failsafe_binary_value(restored_value);
        entry->active = 0U;
        entry->last_reason = FAILSAFE_REASON_NONE;
        entry->last_applied_value = *out_value;
        if (out_origin)
            *out_origin = "snapshot";
        return true;
    }

    action = failsafe_action_for_reason(entry, boot_reason);
    value = failsafe_apply_action(action, restored_value, restored_value, entry->policy.safe_value);

    *out_value = failsafe_binary_value(value);
    entry->last_applied_value = *out_value;
    entry->last_reason = boot_reason;
    entry->active = (entry->policy.recovery_mode == (uint8_t)FAILSAFE_RECOVERY_MANUAL) ? 1U : 0U;

    if (out_origin)
        *out_origin = failsafe_reason_name(boot_reason);

    return true;
}

bool failsafe_guard_command(uint16_t output_id,
                            int32_t requested_value,
                            failsafe_command_source_t source,
                            int32_t *out_effective_value,
                            const char **out_reason)
{
    failsafe_runtime_entry_t *entry = failsafe_find(output_id);

    (void)source;

    if (!entry)
        return false;

    if (out_effective_value)
        *out_effective_value = failsafe_binary_value(requested_value);

    if (entry->policy.enabled &&
        entry->active &&
        entry->policy.recovery_mode == (uint8_t)FAILSAFE_RECOVERY_MANUAL)
    {
        if (out_reason)
            *out_reason = failsafe_reason_name(entry->last_reason);
        return false;
    }

    entry->last_applied_value = failsafe_binary_value(requested_value);

    if (out_reason)
        *out_reason = "command_allowed";

    return true;
}

bool failsafe_apply_for_reason(uint16_t output_id,
                               failsafe_reason_t reason,
                               int32_t *out_safe_value)
{
    failsafe_runtime_entry_t *entry = failsafe_find(output_id);
    failsafe_action_t action;
    int32_t current_value = 0;
    int32_t applied_value;

    if (!entry || !entry->policy.enabled || reason <= FAILSAFE_REASON_NONE || reason >= FAILSAFE_REASON_MAX)
        return false;

    (void)state_get_int(output_id, &current_value);

    action = failsafe_action_for_reason(entry, reason);
    applied_value = failsafe_apply_action(action,
                                          current_value,
                                          current_value,
                                          entry->policy.safe_value);

    entry->last_applied_value = failsafe_binary_value(applied_value);
    entry->last_reason = reason;
    entry->active = (entry->policy.recovery_mode == (uint8_t)FAILSAFE_RECOVERY_MANUAL) ? 1U : 0U;

    if (out_safe_value)
        *out_safe_value = entry->last_applied_value;

    return true;
}

int failsafe_apply_for_reason_all(failsafe_reason_t reason)
{
    int applied = 0;

    for (uint16_t i = 0U; i < entry_count; i++)
    {
        int32_t safe_value = 0;
        uint16_t output_id = entries[i].policy.output_id;

        if (!failsafe_apply_for_reason(output_id, reason, &safe_value))
            continue;

        (void)state_set_int(output_id, safe_value);
        applied++;
    }

    if (applied > 0)
    {
        ESP_LOGW(TAG,
                 "Fail-safe aplicado em %d saida(s), reason=%s",
                 applied,
                 failsafe_reason_name(reason));
    }

    return applied;
}

bool failsafe_clear_manual_reset(uint16_t output_id)
{
    failsafe_runtime_entry_t *entry = failsafe_find(output_id);

    if (!entry)
        return false;

    entry->active = 0U;
    entry->last_reason = FAILSAFE_REASON_NONE;
    return true;
}

bool failsafe_is_active(uint16_t output_id)
{
    const failsafe_runtime_entry_t *entry = failsafe_find_const(output_id);

    return entry && entry->active != 0U;
}

bool failsafe_trigger_comm_loss(uint16_t output_id, int32_t *out_safe_value)
{
    return failsafe_apply_for_reason(output_id, FAILSAFE_REASON_COMM_LOSS, out_safe_value);
}

bool failsafe_trigger_runtime_fault(uint16_t output_id, int32_t *out_safe_value)
{
    return failsafe_apply_for_reason(output_id, FAILSAFE_REASON_RUNTIME_FAULT, out_safe_value);
}

bool failsafe_trigger_manual_test(uint16_t output_id, int32_t *out_safe_value)
{
    return failsafe_apply_for_reason(output_id, FAILSAFE_REASON_MANUAL_TEST, out_safe_value);
}

bool failsafe_rearm(uint16_t output_id)
{
    return failsafe_clear_manual_reset(output_id);
}


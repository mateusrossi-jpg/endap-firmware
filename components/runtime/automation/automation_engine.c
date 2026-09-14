#include "automation_engine.h"
#include "automation_node.h"
#include "cluster_io.h"
#include "cluster_manager.h"
#include "cluster_metrics.h"
#include "cluster_transport.h"
#include "device_profile.h"
#include "failsafe.h"
#include "node_registry.h"
#include "protocol.h"

#include "event_bus.h"
#include "state.h"
#include "pve.h"
#include "esp_attr.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "nvs.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"

#include <inttypes.h>
#include <string.h>
#include <stdint.h>
#include "endap_nvs.h"

/* ============================================================
   CONFIG
============================================================ */

#define TAG "AUTOMATION"
#define AUTOMATION_NAMESPACE "automation"
#define AUTOMATION_KEY_RULES  "rules"
#define AUTOMATION_MAGIC      0x4155544FU
#define AUTOMATION_VERSION    4U
#define AUTOMATION_VERSION_V3 3U
#define AUTOMATION_VERSION_V2 2U
#define AUTOMATION_VERSION_V1 1U

typedef struct
{
    uint32_t magic;
    uint16_t version;
    uint16_t count;
    automation_node_t nodes[AUTOMATION_ENGINE_MAX_NODES];
    automation_interlock_t interlocks[AUTOMATION_ENGINE_MAX_NODES];
    uint32_t crc;
} automation_blob_t;

typedef struct
{
    uint32_t magic;
    uint16_t version;
    uint16_t count;
    automation_node_t nodes[AUTOMATION_ENGINE_MAX_NODES];
    uint32_t crc;
} automation_blob_v3_t;

typedef struct
{
    uint16_t input;
    uint16_t output;
    int32_t threshold;
} automation_node_v1_t;

typedef struct
{
    uint16_t input;
    uint16_t output;
    int32_t threshold;
    uint8_t op;
    int8_t on_true;
    int8_t on_false;
    uint8_t reserved;
} automation_node_v2_t;

typedef struct
{
    uint32_t magic;
    uint16_t version;
    uint16_t count;
    automation_node_v1_t nodes[AUTOMATION_ENGINE_MAX_NODES];
    uint32_t crc;
} automation_blob_v1_t;

typedef struct
{
    uint32_t magic;
    uint16_t version;
    uint16_t count;
    automation_node_v2_t nodes[AUTOMATION_ENGINE_MAX_NODES];
    uint32_t crc;
} automation_blob_v2_t;

/* ============================================================
   STORAGE
============================================================ */

static automation_node_t nodes[AUTOMATION_ENGINE_MAX_NODES];
static automation_interlock_t interlocks[AUTOMATION_ENGINE_MAX_NODES];
static int node_count = 0;
static bool persisted_config = false;
static uint32_t automation_tick_ms = 0;
static automation_rule_diag_t rule_diags[AUTOMATION_ENGINE_MAX_NODES];

typedef struct
{
    uint8_t condition;
    uint8_t timer_active;
    uint8_t pulse_active;
    uint8_t latched_true;
    uint32_t deadline_ms;
} automation_runtime_t;

static automation_runtime_t runtime_nodes[AUTOMATION_ENGINE_MAX_NODES];

typedef struct
{
    uint8_t op;
    const char *code;
    const char *symbol;
} automation_operator_info_t;

static const automation_operator_info_t operator_table[] =
{
    {AUTOMATION_OP_GT, "gt", ">"},
    {AUTOMATION_OP_GE, "ge", ">="},
    {AUTOMATION_OP_EQ, "eq", "=="},
    {AUTOMATION_OP_NE, "ne", "!="},
    {AUTOMATION_OP_LE, "le", "<="},
    {AUTOMATION_OP_LT, "lt", "<"},
};

typedef struct
{
    uint8_t mode;
    const char *code;
    const char *label;
} automation_mode_info_t;

static const automation_mode_info_t mode_table[] =
{
    {AUTOMATION_MODE_FOLLOW, "follow", "FOLLOW"},
    {AUTOMATION_MODE_PULSE_MS, "pulse_ms", "PULSE_MS"},
    {AUTOMATION_MODE_ON_DELAY_MS, "on_delay_ms", "ON_DELAY_MS"},
    {AUTOMATION_MODE_OFF_DELAY_MS, "off_delay_ms", "OFF_DELAY_MS"},
    {AUTOMATION_MODE_TOGGLE, "toggle", "TOGGLE"},
    {AUTOMATION_MODE_FORCE_ON, "force_on", "FORCE_ON"},
    {AUTOMATION_MODE_FORCE_OFF, "force_off", "FORCE_OFF"},
};

/* ============================================================
   DISPATCH QUEUE E TASK
============================================================ */

typedef struct
{
    uint32_t target_node;
    uint32_t requester_node;
    uint16_t output_id;
    int32_t value;
} automation_remote_cmd_t;

static QueueHandle_t automation_remote_queue = NULL;

static void automation_dispatcher_task(void *arg)
{
    automation_remote_cmd_t cmd;
    protocol_msg_t msg = {0};

    while (1)
    {
        if (xQueueReceive(automation_remote_queue, &cmd, portMAX_DELAY) == pdTRUE)
        {
            if (!cluster_transport_is_ready())
                continue;

            msg.type = PROTOCOL_MSG_OUTPUT_COMMAND;
            msg.data.output_command.target_node = cmd.target_node;
            msg.data.output_command.requester_node = cmd.requester_node;
            msg.data.output_command.output_id = cmd.output_id;
            msg.data.output_command.value = cmd.value;

            cluster_transport_broadcast_frame((const uint8_t *)&msg, sizeof(msg));
        }
    }
}

/* ============================================================
   CRC
============================================================ */

static uint32_t automation_crc32(const uint8_t *data, size_t len)
{
    uint32_t crc = 2166136261u;

    for (size_t i = 0; i < len; i++)
    {
        crc ^= data[i];
        crc *= 16777619u;
    }

    return crc;
}

static bool automation_valid_input(int input)
{
    return device_profile_is_valid_input((uint16_t)input);
}

static bool automation_valid_output(int output)
{
    return device_profile_is_valid_output((uint16_t)output);
}

static bool automation_valid_action(int value)
{
    return (value == 0) || (value == 1);
}

static bool automation_valid_operator(uint8_t op)
{
    return op < AUTOMATION_OP_MAX;
}

static bool automation_valid_mode(uint8_t mode)
{
    return mode < AUTOMATION_MODE_MAX;
}

static bool automation_rules_equal(const automation_node_t *a, const automation_node_t *b)
{
    return a->input == b->input &&
           a->output == b->output &&
           a->threshold == b->threshold &&
           a->op == b->op &&
           a->mode == b->mode &&
           a->duration_ms == b->duration_ms &&
           a->on_true == b->on_true &&
           a->on_false == b->on_false;
}

static bool automation_rule_is_valid(const automation_node_t *node)
{
    if (node == NULL)
        return false;

    return automation_valid_input(node->input) &&
           automation_valid_output(node->output) &&
           automation_valid_operator(node->op) &&
           automation_valid_mode(node->mode) &&
           automation_valid_action(node->on_true) &&
           automation_valid_action(node->on_false) &&
           (((node->mode == AUTOMATION_MODE_PULSE_MS) ||
             (node->mode == AUTOMATION_MODE_ON_DELAY_MS) ||
             (node->mode == AUTOMATION_MODE_OFF_DELAY_MS)) ?
                (node->duration_ms > 0) : true);
}

static bool automation_mode_is_force_pair(uint8_t mode)
{
    return (mode == AUTOMATION_MODE_FORCE_ON) ||
           (mode == AUTOMATION_MODE_FORCE_OFF);
}

static bool automation_rule_conflicts(const automation_node_t *existing, const automation_node_t *candidate)
{
    if (existing->output != candidate->output ||
        automation_rules_equal(existing, candidate))
    {
        return false;
    }

    if (automation_mode_is_force_pair(existing->mode) &&
        automation_mode_is_force_pair(candidate->mode))
    {
        return existing->mode == candidate->mode;
    }

    return true;
}

bool automation_engine_operator_from_code(const char *text, uint8_t *out_operator)
{
    if (!text || !out_operator)
        return false;

    for (size_t i = 0; i < (sizeof(operator_table) / sizeof(operator_table[0])); i++)
    {
        if (strcmp(text, operator_table[i].code) == 0 ||
            strcmp(text, operator_table[i].symbol) == 0)
        {
            *out_operator = operator_table[i].op;
            return true;
        }
    }

    return false;
}

bool automation_engine_mode_from_code(const char *text, uint8_t *out_mode)
{
    if (!text || !out_mode)
        return false;

    for (size_t i = 0; i < (sizeof(mode_table) / sizeof(mode_table[0])); i++)
    {
        if (strcmp(text, mode_table[i].code) == 0 ||
            strcmp(text, mode_table[i].label) == 0)
        {
            *out_mode = mode_table[i].mode;
            return true;
        }
    }

    return false;
}

const char *automation_engine_operator_to_code(uint8_t op)
{
    for (size_t i = 0; i < (sizeof(operator_table) / sizeof(operator_table[0])); i++)
    {
        if (operator_table[i].op == op)
            return operator_table[i].code;
    }

    return operator_table[0].code;
}

const char *automation_engine_operator_to_symbol(uint8_t op)
{
    for (size_t i = 0; i < (sizeof(operator_table) / sizeof(operator_table[0])); i++)
    {
        if (operator_table[i].op == op)
            return operator_table[i].symbol;
    }

    return operator_table[0].symbol;
}

const char *automation_engine_mode_to_code(uint8_t mode)
{
    for (size_t i = 0; i < (sizeof(mode_table) / sizeof(mode_table[0])); i++)
    {
        if (mode_table[i].mode == mode)
            return mode_table[i].code;
    }

    return mode_table[0].code;
}

const char *automation_engine_mode_to_label(uint8_t mode)
{
    for (size_t i = 0; i < (sizeof(mode_table) / sizeof(mode_table[0])); i++)
    {
        if (mode_table[i].mode == mode)
            return mode_table[i].label;
    }

    return mode_table[0].label;
}

static bool automation_eval_rule(const automation_node_t *node, int32_t value)
{
    switch (node->op)
    {
        case AUTOMATION_OP_GT: return value > node->threshold;
        case AUTOMATION_OP_GE: return value >= node->threshold;
        case AUTOMATION_OP_EQ: return value == node->threshold;
        case AUTOMATION_OP_NE: return value != node->threshold;
        case AUTOMATION_OP_LE: return value <= node->threshold;
        case AUTOMATION_OP_LT: return value < node->threshold;
        default: return false;
    }
}

static bool automation_eval_condition(const automation_condition_t *cond)
{
    int32_t val = 0;
    if (!state_get_int(cond->channel, &val))
        return false;

    switch (cond->op)
    {
        case AUTOMATION_OP_EQ: return val == (int32_t)cond->target_val;
        case AUTOMATION_OP_NE: return val != (int32_t)cond->target_val;
        case AUTOMATION_OP_GT: return val > (int32_t)cond->target_val;
        case AUTOMATION_OP_GE: return val >= (int32_t)cond->target_val;
        case AUTOMATION_OP_LE: return val <= (int32_t)cond->target_val;
        case AUTOMATION_OP_LT: return val < (int32_t)cond->target_val;
        default: return val == (int32_t)cond->target_val;
    }
}

static bool automation_eval_interlock(const automation_interlock_t *interlock)
{
    if (!interlock || !interlock->enabled || interlock->cond_count == 0)
        return true; /* Sem interlock ativo -> permissão concedida (regra legada / padrão) */

    if (interlock->cond_count == 1)
        return automation_eval_condition(&interlock->conditions[0]);

    bool c0 = automation_eval_condition(&interlock->conditions[0]);
    bool c1 = automation_eval_condition(&interlock->conditions[1]);

    if (interlock->logic_op == 1) /* OR */
        return c0 || c1;

    /* Default: 0 = AND */
    return c0 && c1;
}

static uint32_t automation_deadline_after(uint16_t duration_ms)
{
    return automation_tick_ms + (uint32_t)duration_ms;
}

static bool automation_time_reached(uint32_t deadline_ms)
{
    return (int32_t)(automation_tick_ms - deadline_ms) >= 0;
}

static uint32_t automation_now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000ULL);
}

static cluster_node_state_t automation_owner_state(uint32_t node_id)
{
    cluster_node_t snapshot[MAX_NODES];
    int count;

    if (node_id == 0U)
        return CLUSTER_NODE_OFFLINE;

    count = cluster_manager_export_nodes(snapshot, MAX_NODES);
    for (int i = 0; i < count; i++)
    {
        if (snapshot[i].node_id == node_id)
            return snapshot[i].state;
    }

    return CLUSTER_NODE_OFFLINE;
}

static void automation_diag_mark_eval(int index, bool condition_now, uint32_t trigger_ms)
{
    if (index < 0 || index >= AUTOMATION_ENGINE_MAX_NODES)
        return;

    rule_diags[index].last_trigger_ms = trigger_ms;
    rule_diags[index].last_eval_ms = trigger_ms;
    rule_diags[index].last_condition = condition_now ? 1U : 0U;
}

static void automation_diag_mark_action(int index,
                                        int32_t value,
                                        automation_action_result_t result,
                                        bool target_local,
                                        uint32_t owner,
                                        uint32_t original_owner)
{
    if (index < 0 || index >= AUTOMATION_ENGINE_MAX_NODES)
        return;

    rule_diags[index].last_action_ms = automation_now_ms();
    rule_diags[index].last_action_value = (uint8_t)(value != 0 ? 1 : 0);
    rule_diags[index].last_action_result = (uint8_t)result;
    rule_diags[index].last_target_local = target_local ? 1U : 0U;
    rule_diags[index].last_target_owner = owner;
    rule_diags[index].last_target_original_owner = original_owner;
}

static automation_action_result_t automation_dispatch_output(uint16_t output,
                                                             int32_t value,
                                                             int rule_index)
{
    cluster_metrics_t metrics = cluster_get_metrics();
    uint32_t owner = cluster_io_get_owner(output);
    uint32_t original_owner = cluster_io_get_original_owner(output);
    bool target_local = cluster_io_is_local(output);

    if (target_local)
    {
        int32_t effective_value = value;
        const char *failsafe_reason = NULL;

        if (!failsafe_guard_command(output,
                                    value,
                                    FAILSAFE_COMMAND_AUTOMATION,
                                    &effective_value,
                                    &failsafe_reason))
        {
            automation_diag_mark_action(rule_index,
                                        value,
                                        AUTOMATION_ACTION_BLOCKED_FAILSAFE,
                                        true,
                                        owner,
                                        original_owner);
            return AUTOMATION_ACTION_BLOCKED_FAILSAFE;
        }

        state_set_int(output, effective_value);
        automation_diag_mark_action(rule_index,
                                    effective_value,
                                    AUTOMATION_ACTION_APPLIED_LOCAL,
                                    true,
                                    owner,
                                    original_owner);
        return AUTOMATION_ACTION_APPLIED_LOCAL;
    }

    if (metrics.self_node == 0U || owner == 0U)
    {
        automation_diag_mark_action(rule_index,
                                    value,
                                    AUTOMATION_ACTION_TARGET_UNRESOLVED,
                                    false,
                                    owner,
                                    original_owner);
        return AUTOMATION_ACTION_TARGET_UNRESOLVED;
    }

    if (automation_owner_state(owner) == CLUSTER_NODE_OFFLINE)
    {
        automation_diag_mark_action(rule_index,
                                    value,
                                    AUTOMATION_ACTION_REMOTE_OWNER_OFFLINE,
                                    false,
                                    owner,
                                    original_owner);
        return AUTOMATION_ACTION_REMOTE_OWNER_OFFLINE;
    }

    if (automation_remote_queue != NULL)
    {
        automation_remote_cmd_t cmd = {
            .target_node = owner,
            .requester_node = metrics.self_node,
            .output_id = output,
            .value = value
        };

        if (xQueueSend(automation_remote_queue, &cmd, 0) != pdTRUE)
        {
            automation_diag_mark_action(rule_index,
                                        value,
                                        AUTOMATION_ACTION_DISPATCH_FAILED,
                                        false,
                                        owner,
                                        original_owner);
            return AUTOMATION_ACTION_DISPATCH_FAILED;
        }
    }
    else
    {
        automation_diag_mark_action(rule_index,
                                    value,
                                    AUTOMATION_ACTION_DISPATCH_FAILED,
                                    false,
                                    owner,
                                    original_owner);
        return AUTOMATION_ACTION_DISPATCH_FAILED;
    }

    automation_diag_mark_action(rule_index,
                                value,
                                AUTOMATION_ACTION_DISPATCHED_REMOTE,
                                false,
                                owner,
                                original_owner);
    return AUTOMATION_ACTION_DISPATCHED_REMOTE;
}

static void automation_apply_output(const automation_node_t *node,
                                    int rule_index,
                                    bool condition_true)
{
    automation_dispatch_output(node->output,
                               condition_true ? node->on_true : node->on_false,
                               rule_index);
}

static void automation_toggle_output(uint16_t output, int rule_index)
{
    int32_t current = 0;

    if (!state_get_int(output, &current))
        current = 0;

    automation_dispatch_output(output, current ? 0 : 1, rule_index);
}

const char *automation_engine_action_result_name(uint8_t result)
{
    switch ((automation_action_result_t)result)
    {
        case AUTOMATION_ACTION_APPLIED_LOCAL:
            return "applied-local";
        case AUTOMATION_ACTION_DISPATCHED_REMOTE:
            return "dispatched-remote";
        case AUTOMATION_ACTION_DISPATCH_FAILED:
            return "dispatch-failed";
        case AUTOMATION_ACTION_REMOTE_OWNER_OFFLINE:
            return "remote-owner-offline";
        case AUTOMATION_ACTION_TARGET_UNRESOLVED:
            return "target-unresolved";
        case AUTOMATION_ACTION_BLOCKED_FAILSAFE:
            return "blocked-failsafe";
        case AUTOMATION_ACTION_IDLE:
        default:
            return "idle";
    }
}

static void automation_handle_output_command(const protocol_output_command_t *msg)
{
    cluster_metrics_t metrics;
    bool requester_known = false;

    if (!msg)
        return;

    metrics = cluster_get_metrics();
    if (metrics.self_node == 0U || msg->target_node != metrics.self_node)
        return;

    if (msg->requester_node != 0U &&
        msg->requester_node != metrics.self_node)
    {
        requester_known = node_registry_is_known(msg->requester_node);
        if (!requester_known)
        {
            ESP_LOGW(TAG,
                     "Comando remoto ignorado: requester %" PRIu32 " ainda nao foi descoberto no registry local",
                     msg->requester_node);
            return;
        }
    }

    if (!device_profile_is_valid_output(msg->output_id))
    {
        ESP_LOGW(TAG,
                 "Comando remoto ignorado: output %u nao existe no perfil local",
                 msg->output_id);
        return;
    }

    /*
     * O target explicito do protocolo e a fonte de verdade para comandos
     * manuais remotos. Os IDs de saida podem se repetir entre nos
     * (ex.: 100/101/102 no gateway e no field node), entao validar
     * "localidade" pela tabela global de ownership pode descartar
     * comandos legitimos ao no correto. Aqui basta o no ser o alvo
     * explicitamente enderecado e a saida existir no perfil local.
     */
    int32_t effective_value = msg->value;
    const char *failsafe_reason = NULL;

    if (!failsafe_guard_command(msg->output_id,
                                msg->value,
                                FAILSAFE_COMMAND_REMOTE,
                                &effective_value,
                                &failsafe_reason))
    {
        ESP_LOGW(TAG,
                 "Comando remoto bloqueado por fail-safe: output=%u requester=%" PRIu32 " reason=%s",
                 msg->output_id,
                 msg->requester_node,
                 failsafe_reason ? failsafe_reason : "unknown");
        return;
    }

    if (!state_set_int(msg->output_id, effective_value))
    {
        ESP_LOGW(TAG,
                 "Falha ao aplicar comando remoto: output=%u value=%" PRId32 " requester=%" PRIu32,
                 msg->output_id,
                 effective_value,
                 msg->requester_node);
        return;
    }

    ESP_LOGI(TAG,
             "Comando remoto aplicado: output=%u value=%" PRId32 " requester=%" PRIu32,
             msg->output_id,
             effective_value,
             msg->requester_node);
}

/* ============================================================
   NVS
============================================================ */

static void automation_persist(void)
{
    automation_blob_t blob = {0};
    nvs_handle_t nvs;

    blob.magic = AUTOMATION_MAGIC;
    blob.version = AUTOMATION_VERSION;
    blob.count = (uint16_t)node_count;

    if (node_count > 0)
    {
        memcpy(blob.nodes, nodes, sizeof(nodes[0]) * (size_t)node_count);
        memcpy(blob.interlocks, interlocks, sizeof(interlocks[0]) * (size_t)node_count);
    }

    blob.crc = automation_crc32((const uint8_t *)&blob, sizeof(blob) - sizeof(blob.crc));

    if (nvs_open(AUTOMATION_NAMESPACE, NVS_READWRITE, &nvs) != ESP_OK)
    {
        ESP_LOGE(TAG, "Falha ao abrir NVS para persistir automacao");
        return;
    }

    if (nvs_set_blob(nvs, AUTOMATION_KEY_RULES, &blob, sizeof(blob)) == ESP_OK &&
        endap_nvs_commit(nvs) == ESP_OK)
    {
        persisted_config = true;
        ESP_LOGI(TAG, "Automacao persistida (%d regra(s) com interlock)", node_count);
    }
    else
    {
        ESP_LOGE(TAG, "Falha ao gravar automacao no NVS");
    }

    nvs_close(nvs);
}

static void automation_load(void)
{
    nvs_handle_t nvs;
    size_t len = 0;

    if (nvs_open(AUTOMATION_NAMESPACE, NVS_READONLY, &nvs) != ESP_OK)
        return;

    if (nvs_get_blob(nvs, AUTOMATION_KEY_RULES, NULL, &len) != ESP_OK)
    {
        nvs_close(nvs);
        return;
    }

    if (len == sizeof(automation_blob_t))
    {
        automation_blob_t blob = {0};
        size_t read_len = sizeof(blob);
        uint32_t crc;

        if (nvs_get_blob(nvs, AUTOMATION_KEY_RULES, &blob, &read_len) != ESP_OK)
        {
            nvs_close(nvs);
            return;
        }

        nvs_close(nvs);

        crc = automation_crc32((const uint8_t *)&blob, sizeof(blob) - sizeof(blob.crc));

        if (blob.magic != AUTOMATION_MAGIC ||
            blob.version != AUTOMATION_VERSION ||
            blob.count > AUTOMATION_ENGINE_MAX_NODES ||
            blob.crc != crc)
        {
            ESP_LOGW(TAG, "Blob de automacao invalido");
            return;
        }

        for (uint16_t i = 0; i < blob.count; i++)
        {
            if (!automation_rule_is_valid(&blob.nodes[i]))
            {
                ESP_LOGW(TAG, "Regra de automacao invalida no NVS");
                return;
            }

            for (uint16_t j = 0; j < i; j++)
            {
                if (automation_rule_conflicts(&blob.nodes[j], &blob.nodes[i]))
                {
                    ESP_LOGW(TAG, "Conflito de automacao no NVS");
                    return;
                }
            }
        }

        if (blob.count > 0)
        {
            memcpy(nodes, blob.nodes, sizeof(nodes[0]) * blob.count);
            memcpy(interlocks, blob.interlocks, sizeof(interlocks[0]) * blob.count);
        }

        node_count = blob.count;
        persisted_config = true;

        ESP_LOGI(TAG, "Automacao restaurada do NVS (%d regra(s) com interlock)", node_count);
        return;
    }

    if (len == sizeof(automation_blob_v3_t))
    {
        automation_blob_v3_t blob = {0};
        size_t read_len = sizeof(blob);
        uint32_t crc;

        if (nvs_get_blob(nvs, AUTOMATION_KEY_RULES, &blob, &read_len) != ESP_OK)
        {
            nvs_close(nvs);
            return;
        }

        nvs_close(nvs);

        crc = automation_crc32((const uint8_t *)&blob, sizeof(blob) - sizeof(blob.crc));

        if (blob.magic != AUTOMATION_MAGIC ||
            blob.version != AUTOMATION_VERSION_V3 ||
            blob.count > AUTOMATION_ENGINE_MAX_NODES ||
            blob.crc != crc)
        {
            ESP_LOGW(TAG, "Blob v3 de automacao invalido");
            return;
        }

        for (uint16_t i = 0; i < blob.count; i++)
        {
            if (!automation_rule_is_valid(&blob.nodes[i]))
            {
                ESP_LOGW(TAG, "Regra v3 de automacao invalida no NVS");
                return;
            }

            for (uint16_t j = 0; j < i; j++)
            {
                if (automation_rule_conflicts(&blob.nodes[j], &blob.nodes[i]))
                {
                    ESP_LOGW(TAG, "Conflito v3 de automacao no NVS");
                    return;
                }
            }
        }

        if (blob.count > 0)
        {
            memcpy(nodes, blob.nodes, sizeof(nodes[0]) * blob.count);
            memset(interlocks, 0, sizeof(interlocks[0]) * blob.count);
        }

        node_count = blob.count;
        persisted_config = true;

        ESP_LOGI(TAG, "Automacao v3 migrada do NVS (%d regra(s))", node_count);
        automation_persist();
        return;
    }

    if (len == sizeof(automation_blob_v2_t))
    {
        automation_blob_v2_t blob = {0};
        size_t read_len = sizeof(blob);
        uint32_t crc;

        if (nvs_get_blob(nvs, AUTOMATION_KEY_RULES, &blob, &read_len) != ESP_OK)
        {
            nvs_close(nvs);
            return;
        }

        nvs_close(nvs);

        crc = automation_crc32((const uint8_t *)&blob, sizeof(blob) - sizeof(blob.crc));

        if (blob.magic != AUTOMATION_MAGIC ||
            blob.version != AUTOMATION_VERSION_V2 ||
            blob.count > AUTOMATION_ENGINE_MAX_NODES ||
            blob.crc != crc)
        {
            ESP_LOGW(TAG, "Blob v2 de automacao invalido");
            return;
        }

        for (uint16_t i = 0; i < blob.count; i++)
        {
            automation_node_t migrated = {
                .input = blob.nodes[i].input,
                .output = blob.nodes[i].output,
                .threshold = blob.nodes[i].threshold,
                .op = blob.nodes[i].op,
                .mode = AUTOMATION_MODE_FOLLOW,
                .duration_ms = 0,
                .on_true = blob.nodes[i].on_true,
                .on_false = blob.nodes[i].on_false
            };

            if (!automation_rule_is_valid(&migrated))
            {
                ESP_LOGW(TAG, "Regra v2 invalida no NVS");
                return;
            }

            for (uint16_t j = 0; j < i; j++)
            {
                if (automation_rule_conflicts(&nodes[j], &migrated))
                {
                    ESP_LOGW(TAG, "Conflito v2 de automacao no NVS");
                    return;
                }
            }

            nodes[i] = migrated;
            memset(&interlocks[i], 0, sizeof(interlocks[0]));
        }

        node_count = blob.count;
        persisted_config = true;

        ESP_LOGI(TAG, "Automacao v2 migrada do NVS (%d regra(s))", node_count);
        automation_persist();
        return;
    }

    if (len == sizeof(automation_blob_v1_t))
    {
        automation_blob_v1_t blob = {0};
        size_t read_len = sizeof(blob);
        uint32_t crc;

        if (nvs_get_blob(nvs, AUTOMATION_KEY_RULES, &blob, &read_len) != ESP_OK)
        {
            nvs_close(nvs);
            return;
        }

        nvs_close(nvs);

        crc = automation_crc32((const uint8_t *)&blob, sizeof(blob) - sizeof(blob.crc));

        if (blob.magic != AUTOMATION_MAGIC ||
            blob.version != AUTOMATION_VERSION_V1 ||
            blob.count > AUTOMATION_ENGINE_MAX_NODES ||
            blob.crc != crc)
        {
            ESP_LOGW(TAG, "Blob legado de automacao invalido");
            return;
        }

        for (uint16_t i = 0; i < blob.count; i++)
        {
            automation_node_t migrated = {
                .input = blob.nodes[i].input,
                .output = blob.nodes[i].output,
                .threshold = blob.nodes[i].threshold,
                .op = AUTOMATION_OP_GT,
                .mode = AUTOMATION_MODE_FOLLOW,
                .duration_ms = 0,
                .on_true = 1,
                .on_false = 0
            };

            if (!automation_rule_is_valid(&migrated))
            {
                ESP_LOGW(TAG, "Regra legada invalida no NVS");
                return;
            }

            for (uint16_t j = 0; j < i; j++)
            {
                if (automation_rule_conflicts(&nodes[j], &migrated))
                {
                    ESP_LOGW(TAG, "Conflito legado de automacao no NVS");
                    return;
                }
            }

            nodes[i] = migrated;
            memset(&interlocks[i], 0, sizeof(interlocks[0]));
        }

        node_count = blob.count;
        persisted_config = true;

        ESP_LOGI(TAG, "Automacao legada migrada do NVS (%d regra(s))", node_count);
        automation_persist();
        return;
    }

    nvs_close(nvs);
    ESP_LOGW(TAG, "Blob de automacao com tamanho invalido");
}

/* ============================================================
   EVENT HANDLER
============================================================ */

static void automation_event_handler(const endap_event_t *ev)
{
    uint32_t trigger_ms;

    if(ev->type != EVENT_STATE_CHANGE)
        return;

    trigger_ms = automation_now_ms();

    /* 1. Avaliação de Trigger para regras onde n->input == ev->source */
    for(int i = 0; i < node_count; i++)
    {
        automation_node_t *n = &nodes[i];
        automation_runtime_t *rt = &runtime_nodes[i];
        automation_interlock_t *ilk = &interlocks[i];
        bool condition_now;
        bool rising_edge;

        if(n->input != ev->source)
            continue;

        int32_t eval_val = pve_get_scaled_value(ev->source, ev->data);
        condition_now = automation_eval_rule(n, eval_val);
        rising_edge = condition_now && !rt->condition;
        rt->condition = condition_now ? 1U : 0U;
        automation_diag_mark_eval(i, condition_now, trigger_ms);

        /* Se for acionar a saída (condição verdadeira ou subida), checar permissão de Interlock */
        if (condition_now || rising_edge)
        {
            if (!automation_eval_interlock(ilk))
            {
                /* Permissão negada por interlock -> bloqueia comando e registra diagnóstico */
                automation_diag_mark_action(i,
                                            n->on_true,
                                            AUTOMATION_ACTION_BLOCKED_FAILSAFE,
                                            cluster_io_is_local(n->output),
                                            cluster_io_get_owner(n->output),
                                            cluster_io_get_original_owner(n->output));
                continue;
            }
        }

        switch (n->mode)
        {
            case AUTOMATION_MODE_FOLLOW:
                automation_apply_output(n, i, condition_now);
                rt->timer_active = 0;
                rt->pulse_active = 0;
                rt->latched_true = condition_now ? 1U : 0U;
                break;

            case AUTOMATION_MODE_PULSE_MS:
                if (rising_edge)
                {
                    automation_apply_output(n, i, true);
                    rt->pulse_active = 1;
                    rt->timer_active = 1;
                    rt->deadline_ms = automation_deadline_after(n->duration_ms);
                }
                break;

            case AUTOMATION_MODE_ON_DELAY_MS:
                if (condition_now)
                {
                    rt->latched_true = 0;
                    rt->timer_active = 1;
                    rt->deadline_ms = automation_deadline_after(n->duration_ms);
                    automation_apply_output(n, i, false);
                }
                else
                {
                    rt->timer_active = 0;
                    rt->latched_true = 0;
                    automation_apply_output(n, i, false);
                }
                break;

            case AUTOMATION_MODE_OFF_DELAY_MS:
                if (condition_now)
                {
                    rt->timer_active = 0;
                    rt->latched_true = 1;
                    automation_apply_output(n, i, true);
                }
                else
                {
                    rt->timer_active = 1;
                    rt->deadline_ms = automation_deadline_after(n->duration_ms);
                }
                break;

            case AUTOMATION_MODE_TOGGLE:
                if (rising_edge)
                    automation_toggle_output(n->output, i);
                break;

            case AUTOMATION_MODE_FORCE_ON:
                if (rising_edge)
                    automation_dispatch_output(n->output, 1, i);
                break;

            case AUTOMATION_MODE_FORCE_OFF:
                if (rising_edge)
                    automation_dispatch_output(n->output, 0, i);
                break;

            default:
                break;
        }
    }

    /* 2. GUARDA CONTÍNUA DE INTERTRAVAMENTO:
       Se o canal modificado (ev->source) fizer parte de um interlock ativo
       de uma regra cuja saída está atualmente LIGADA, e a permissão caiu,
       desligar a saída imediatamente (estado seguro). */
    for(int i = 0; i < node_count; i++)
    {
        automation_node_t *n = &nodes[i];
        automation_interlock_t *ilk = &interlocks[i];

        if (!ilk->enabled || ilk->cond_count == 0)
            continue;

        bool is_interlock_channel = false;
        for (int c = 0; c < ilk->cond_count; c++)
        {
            if (ilk->conditions[c].channel == ev->source)
            {
                is_interlock_channel = true;
                break;
            }
        }

        if (is_interlock_channel)
        {
            int32_t out_val = 0;
            if (state_get_int(n->output, &out_val) && out_val != 0)
            {
                /* A saída está ligada; verificar se a permissão foi perdida */
                if (!automation_eval_interlock(ilk))
                {
                    automation_runtime_t *rt = &runtime_nodes[i];
                    rt->timer_active = 0;
                    rt->pulse_active = 0;
                    rt->latched_true = 0;

                    automation_dispatch_output(n->output, n->on_false, i);
                }
            }
        }
    }
}

/* ============================================================
   NODE REGISTRATION
============================================================ */

automation_result_t automation_engine_add_rule(
    int input,
    uint8_t op,
    uint8_t mode,
    int threshold,
    uint16_t duration_ms,
    int output,
    int on_true,
    int on_false)
{
    if (!automation_valid_input(input) ||
        !automation_valid_output(output) ||
        !automation_valid_operator(op) ||
        !automation_valid_mode(mode) ||
        !automation_valid_action(on_true) ||
        !automation_valid_action(on_false) ||
        (((mode == AUTOMATION_MODE_PULSE_MS) ||
          (mode == AUTOMATION_MODE_ON_DELAY_MS) ||
          (mode == AUTOMATION_MODE_OFF_DELAY_MS)) && duration_ms == 0))
    {
        return AUTOMATION_RESULT_INVALID;
    }

    automation_node_t candidate = {
        .input = (uint16_t)input,
        .output = (uint16_t)output,
        .threshold = threshold,
        .op = op,
        .mode = mode,
        .duration_ms = duration_ms,
        .on_true = (int8_t)on_true,
        .on_false = (int8_t)on_false
    };

    for (int i = 0; i < node_count; i++)
    {
        if (automation_rules_equal(&nodes[i], &candidate))
            return AUTOMATION_RESULT_DUPLICATE;

        if (automation_rule_conflicts(&nodes[i], &candidate))
            return AUTOMATION_RESULT_CONFLICT;
    }

    if(node_count >= AUTOMATION_ENGINE_MAX_NODES)
        return AUTOMATION_RESULT_FULL;

    nodes[node_count] = candidate;
    memset(&interlocks[node_count], 0, sizeof(interlocks[0]));
    memset(&runtime_nodes[node_count], 0, sizeof(runtime_nodes[0]));
    memset(&rule_diags[node_count], 0, sizeof(rule_diags[0]));
    node_count++;
    automation_persist();

    int32_t current_val = 0;
    if (state_get_int(input, &current_val))
    {
        endap_event_t ev = {
            .type = EVENT_STATE_CHANGE,
            .source = (uint16_t)input,
            .data = current_val
        };
        automation_event_handler(&ev);
    }

    return AUTOMATION_RESULT_OK;
}

bool automation_engine_set_interlock_at(int index, const automation_interlock_t *interlock)
{
    if (index < 0 || index >= node_count)
        return false;

    if (interlock != NULL)
        interlocks[index] = *interlock;
    else
        memset(&interlocks[index], 0, sizeof(interlocks[0]));

    automation_persist();
    return true;
}

bool automation_engine_get_interlock_at(int index, automation_interlock_t *out_interlock)
{
    if (index < 0 || index >= node_count || out_interlock == NULL)
        return false;

    *out_interlock = interlocks[index];
    return true;
}

bool automation_engine_add_node(
    int input,
    int threshold,
    int output)
{
    automation_result_t result = automation_engine_add_rule(
        input,
        AUTOMATION_OP_GT,
        AUTOMATION_MODE_FOLLOW,
        threshold,
        0,
        output,
        1,
        0);

    return (result == AUTOMATION_RESULT_OK) ||
           (result == AUTOMATION_RESULT_DUPLICATE);
}

int automation_engine_get_node_count(void)
{
    return node_count;
}

int automation_engine_export_nodes(automation_node_t *out, int max_nodes)
{
    int copy_count;

    if (!out || max_nodes <= 0)
        return node_count;

    copy_count = (node_count < max_nodes) ? node_count : max_nodes;

    if (copy_count > 0)
        memcpy(out, nodes, sizeof(nodes[0]) * (size_t)copy_count);

    return copy_count;
}

int automation_engine_export_diags(automation_rule_diag_t *out, int max_nodes)
{
    int copy_count;

    if (!out || max_nodes <= 0)
        return node_count;

    copy_count = (node_count < max_nodes) ? node_count : max_nodes;

    if (copy_count > 0)
        memcpy(out, rule_diags, sizeof(rule_diags[0]) * (size_t)copy_count);

    return copy_count;
}

bool automation_engine_remove_node_at(int index)
{
    if (index < 0 || index >= node_count)
        return false;

    for (int i = index; i < (node_count - 1); i++)
    {
        nodes[i] = nodes[i + 1];
        interlocks[i] = interlocks[i + 1];
        runtime_nodes[i] = runtime_nodes[i + 1];
        rule_diags[i] = rule_diags[i + 1];
    }

    if (node_count > 0)
    {
        memset(&nodes[node_count - 1], 0, sizeof(nodes[0]));
        memset(&interlocks[node_count - 1], 0, sizeof(interlocks[0]));
        memset(&runtime_nodes[node_count - 1], 0, sizeof(runtime_nodes[0]));
        memset(&rule_diags[node_count - 1], 0, sizeof(rule_diags[0]));
    }

    node_count--;
    automation_persist();

    return true;
}

void automation_engine_clear(void)
{
    memset(nodes, 0, sizeof(nodes));
    memset(interlocks, 0, sizeof(interlocks));
    memset(runtime_nodes, 0, sizeof(runtime_nodes));
    memset(rule_diags, 0, sizeof(rule_diags));
    node_count = 0;
    automation_persist();
}

bool automation_engine_has_persisted_config(void)
{
    return persisted_config;
}

void IRAM_ATTR automation_engine_tick_1ms(void)
{
    automation_tick_ms++;

    for (int i = 0; i < node_count; i++)
    {
        const automation_node_t *n = &nodes[i];
        automation_runtime_t *rt = &runtime_nodes[i];

        if (!rt->timer_active)
            continue;

        if (!automation_time_reached(rt->deadline_ms))
            continue;

        rt->timer_active = 0;

        switch (n->mode)
        {
            case AUTOMATION_MODE_PULSE_MS:
                if (rt->pulse_active)
                {
                    rt->pulse_active = 0;
                    automation_apply_output(n, i, false);
                }
                break;

            case AUTOMATION_MODE_ON_DELAY_MS:
                if (rt->condition)
                {
                    rt->latched_true = 1;
                    automation_apply_output(n, i, true);
                }
                break;

            case AUTOMATION_MODE_OFF_DELAY_MS:
                if (!rt->condition)
                {
                    rt->latched_true = 0;
                    automation_apply_output(n, i, false);
                }
                break;

            case AUTOMATION_MODE_TOGGLE:
            case AUTOMATION_MODE_FORCE_ON:
            case AUTOMATION_MODE_FORCE_OFF:

            default:
                break;
        }
    }
}

/* ============================================================
   INIT
============================================================ */

void automation_engine_init(void)
{
    memset(nodes, 0, sizeof(nodes));
    memset(runtime_nodes, 0, sizeof(runtime_nodes));
    memset(rule_diags, 0, sizeof(rule_diags));
    node_count = 0;
    persisted_config = false;
    automation_tick_ms = 0;

    if (automation_remote_queue == NULL)
    {
        automation_remote_queue = xQueueCreate(16, sizeof(automation_remote_cmd_t));
        if (automation_remote_queue != NULL)
        {
            xTaskCreate(automation_dispatcher_task, "auto_disp", 3072, NULL, 5, NULL);
        }
    }

    automation_load();

    if (node_count == 0 && !persisted_config)
    {
        ESP_LOGI(TAG, "Nenhuma automação prévia. Inicialização limpa aguardando regras do operador.");
    }

    protocol_register_output_command_callback(automation_handle_output_command);

    event_bus_subscribe(
        EVENT_STATE_CHANGE,
        automation_event_handler);
}

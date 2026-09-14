#include "ladder_engine.h"
#include <string.h>
#include <inttypes.h>
#include "esp_log.h"
#include "state.h"
#include "io_map.h"
#include "esp_attr.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_timer.h"

static const char *TAG = "ladder";

#define LADDER_MAX_PROGRAM_SIZE 2048

static uint8_t s_bytecode[LADDER_MAX_PROGRAM_SIZE];
static size_t s_bytecode_length = 0;
static bool s_has_program = false;

// OPCODES correspondentes ao compilador do Studio
#define OP_LD             0x01
#define OP_LDN            0x02
#define OP_AND            0x03
#define OP_ANDN           0x04
#define OP_OR             0x05
#define OP_ORN            0x06
#define OP_ST             0x10
#define OP_SET            0x11
#define OP_RST            0x12
#define OP_OR_BLOCK_START 0x20
#define OP_OR_BLOCK_NEXT  0x21
#define OP_OR_BLOCK_END   0x22
#define OP_TON            0x30
#define OP_TOF            0x31
#define OP_CTU            0x32
#define OP_CTD            0x33
#define OP_EQU            0x40
#define OP_GRT            0x41
#define OP_LES            0x42
#define OP_ADD            0x50
#define OP_SUB            0x51
#define OP_MUL            0x52
#define OP_DIV            0x53

#define MAX_TIMERS 16
#define MAX_COUNTERS 16
#define MAX_STACK 16

typedef struct {
    uint16_t tag_id;
    bool active;
    bool last_acc;
    int64_t start_time_ms;
} timer_state_t;

typedef struct {
    uint16_t tag_id;
    bool last_acc;
    int32_t current_count;
} counter_state_t;

static timer_state_t s_timers[MAX_TIMERS];
static size_t s_timer_count = 0;
static counter_state_t s_counters[MAX_COUNTERS];
static size_t s_counter_count = 0;

static timer_state_t* get_or_create_timer(uint16_t tag_id) {
    for (size_t i = 0; i < s_timer_count; i++) {
        if (s_timers[i].tag_id == tag_id) return &s_timers[i];
    }
    if (s_timer_count < MAX_TIMERS) {
        s_timers[s_timer_count].tag_id = tag_id;
        s_timers[s_timer_count].active = false;
        s_timers[s_timer_count].last_acc = false;
        s_timers[s_timer_count].start_time_ms = 0;
        return &s_timers[s_timer_count++];
    }
    return NULL;
}

static counter_state_t* get_or_create_counter(uint16_t tag_id) {
    for (size_t i = 0; i < s_counter_count; i++) {
        if (s_counters[i].tag_id == tag_id) return &s_counters[i];
    }
    if (s_counter_count < MAX_COUNTERS) {
        s_counters[s_counter_count].tag_id = tag_id;
        s_counters[s_counter_count].last_acc = false;
        s_counters[s_counter_count].current_count = 0;
        return &s_counters[s_counter_count++];
    }
    return NULL;
}

static void reset_timer_or_counter(uint16_t tag_id)
{
    for (size_t i = 0; i < s_timer_count; i++) {
        if (s_timers[i].tag_id == tag_id) {
            s_timers[i].active = false;
            s_timers[i].last_acc = false;
            s_timers[i].start_time_ms = 0;
            break;
        }
    }
    for (size_t i = 0; i < s_counter_count; i++) {
        if (s_counters[i].tag_id == tag_id) {
            s_counters[i].last_acc = false;
            s_counters[i].current_count = 0;
            break;
        }
    }
}

esp_err_t ladder_engine_init(void)
{
    s_bytecode_length = 0;
    s_has_program = false;
    s_timer_count = 0;
    s_counter_count = 0;
    memset(s_bytecode, 0, sizeof(s_bytecode));
    memset(s_timers, 0, sizeof(s_timers));
    memset(s_counters, 0, sizeof(s_counters));

    // Load from NVS
    nvs_handle_t my_handle;
    esp_err_t err = nvs_open("endap_ladder", NVS_READONLY, &my_handle);
    if (err == ESP_OK) {
        size_t required_size = 0;
        err = nvs_get_blob(my_handle, "bytecode", NULL, &required_size);
        if (err == ESP_OK && required_size > 0 && required_size <= LADDER_MAX_PROGRAM_SIZE) {
            err = nvs_get_blob(my_handle, "bytecode", s_bytecode, &required_size);
            if (err == ESP_OK) {
                s_bytecode_length = required_size;
                s_has_program = true;
                ESP_LOGI(TAG, "Restored %zu bytes of bytecode from NVS.", required_size);
            }
        }
        nvs_close(my_handle);
    } else {
        ESP_LOGI(TAG, "No bytecode found in NVS (or NVS not initialized).");
    }

    ESP_LOGI(TAG, "Ladder Bytecode Engine Initialized.");
    return ESP_OK;
}

esp_err_t ladder_engine_clear(void)
{
    s_has_program = false;
    s_bytecode_length = 0;
    s_timer_count = 0;
    s_counter_count = 0;
    memset(s_bytecode, 0, sizeof(s_bytecode));
    memset(s_timers, 0, sizeof(s_timers));
    memset(s_counters, 0, sizeof(s_counters));

    nvs_handle_t my_handle;
    esp_err_t err = nvs_open("endap_ladder", NVS_READWRITE, &my_handle);
    if (err == ESP_OK) {
        nvs_erase_key(my_handle, "bytecode");
        nvs_commit(my_handle);
        nvs_close(my_handle);
        ESP_LOGI(TAG, "Ladder bytecode cleared from NVS.");
    }
    return ESP_OK;
}

esp_err_t ladder_engine_load_program(const uint8_t *buffer, size_t length)
{
    if (length > LADDER_MAX_PROGRAM_SIZE) {
        return ESP_ERR_NO_MEM;
    }
    
    // Check Header "ENDP"
    if (length < 5 || buffer[0] != 0x45 || buffer[1] != 0x4E || buffer[2] != 0x44 || buffer[3] != 0x50) {
        ESP_LOGE(TAG, "Invalid Bytecode Header");
        return ESP_ERR_INVALID_ARG;
    }

    // Pass 1: Validate opcodes and limit check timers/counters
    size_t pc = 5;
    size_t unique_timers = 0;
    size_t unique_counters = 0;
    uint16_t timer_tags[MAX_TIMERS];
    uint16_t counter_tags[MAX_COUNTERS];

    while (pc < length) {
        uint8_t op = buffer[pc++];
        if (op == 0xFF) break;

        if ((op >= OP_LD && op <= OP_ORN) || (op >= OP_ST && op <= OP_RST)) {
            if (pc + 1 >= length) return ESP_ERR_INVALID_ARG;
            pc += 2;
        } else if (op >= OP_OR_BLOCK_START && op <= OP_OR_BLOCK_END) {
            // Stack instructions (no operand)
        } else if (op == OP_TON || op == OP_TOF) {
            if (pc + 1 >= length) return ESP_ERR_INVALID_ARG;
            uint16_t tag_id = (buffer[pc] << 8) | buffer[pc + 1];
            pc += 2;
            if (pc + 1 >= length) return ESP_ERR_INVALID_ARG; // preset_ms (uint16_t)
            pc += 2;

            bool exists = false;
            for (size_t i = 0; i < unique_timers; i++) {
                if (timer_tags[i] == tag_id) { exists = true; break; }
            }
            if (!exists) {
                if (unique_timers >= MAX_TIMERS) {
                    ESP_LOGE(TAG, "Exceeded MAX_TIMERS limit (%d)", MAX_TIMERS);
                    return ESP_ERR_NO_MEM;
                }
                timer_tags[unique_timers++] = tag_id;
            }
        } else if (op == OP_CTU || op == OP_CTD) {
            if (pc + 1 >= length) return ESP_ERR_INVALID_ARG;
            uint16_t tag_id = (buffer[pc] << 8) | buffer[pc + 1];
            pc += 2;
            if (pc + 1 >= length) return ESP_ERR_INVALID_ARG; // preset_cnt (uint16_t)
            pc += 2;

            bool exists = false;
            for (size_t i = 0; i < unique_counters; i++) {
                if (counter_tags[i] == tag_id) { exists = true; break; }
            }
            if (!exists) {
                if (unique_counters >= MAX_COUNTERS) {
                    ESP_LOGE(TAG, "Exceeded MAX_COUNTERS limit (%d)", MAX_COUNTERS);
                    return ESP_ERR_NO_MEM;
                }
                counter_tags[unique_counters++] = tag_id;
            }
        } else if (op >= OP_EQU && op <= OP_LES) {
            if (pc + 1 >= length) return ESP_ERR_INVALID_ARG;
            pc += 2;
        } else if (op >= OP_ADD && op <= OP_DIV) {
            if (pc + 6 >= length) return ESP_ERR_INVALID_ARG;
            uint8_t flags_b = buffer[pc + 2];
            if (flags_b != 0x00 && flags_b != 0x01) {
                ESP_LOGE(TAG, "Invalid math flags_b 0x%02X at offset %zu", flags_b, pc + 2);
                return ESP_ERR_INVALID_ARG;
            }
            pc += 7;
        } else {
            ESP_LOGE(TAG, "Unknown Opcode 0x%02X at offset %zu", op, pc - 1);
            return ESP_ERR_INVALID_ARG;
        }
    }

    // Pass 2: Save and initialize structures
    ladder_engine_clear();
    memcpy(s_bytecode, buffer, length);
    s_bytecode_length = length;
    s_has_program = true;
    
    // Save to NVS
    nvs_handle_t my_handle;
    esp_err_t err = nvs_open("endap_ladder", NVS_READWRITE, &my_handle);
    if (err == ESP_OK) {
        nvs_set_blob(my_handle, "bytecode", s_bytecode, s_bytecode_length);
        nvs_commit(my_handle);
        nvs_close(my_handle);
        ESP_LOGI(TAG, "Ladder program saved to NVS, size: %zu bytes", length);
    } else {
        ESP_LOGE(TAG, "Failed to open NVS to save bytecode.");
    }

    return ESP_OK;
}

size_t ladder_engine_get_program(uint8_t *out_buf, size_t max_len)
{
    if (!s_has_program || out_buf == NULL || max_len == 0) return 0;
    size_t copy_len = (s_bytecode_length < max_len) ? s_bytecode_length : max_len;
    memcpy(out_buf, s_bytecode, copy_len);
    return copy_len;
}

// Rodar no HOT PATH - IRAM_ATTR para garantir determinismo
void IRAM_ATTR ladder_engine_run(void)
{
    if (!s_has_program || s_bytecode_length < 5) return;

    bool accumulator = false;
    
    // Branch stacks
    bool logic_stack[MAX_STACK];
    bool or_stack[MAX_STACK];
    int stack_ptr = 0;

    size_t pc = 5; // Pula Header (4 bytes) + Versão (1 byte)
    int64_t now_ms = esp_timer_get_time() / 1000;

    while (pc < s_bytecode_length) {
        uint8_t opcode = s_bytecode[pc++];
        
        if (opcode == 0xFF) {
            break; // Terminator
        }

        // Handle no-operand branch stack instructions
        if (opcode == OP_OR_BLOCK_START) {
            if (stack_ptr < MAX_STACK) {
                logic_stack[stack_ptr] = accumulator;
                or_stack[stack_ptr] = false;
                stack_ptr++;
            }
            continue;
        } else if (opcode == OP_OR_BLOCK_NEXT) {
            if (stack_ptr > 0) {
                or_stack[stack_ptr - 1] = or_stack[stack_ptr - 1] || accumulator;
                accumulator = logic_stack[stack_ptr - 1];
            }
            continue;
        } else if (opcode == OP_OR_BLOCK_END) {
            if (stack_ptr > 0) {
                or_stack[stack_ptr - 1] = or_stack[stack_ptr - 1] || accumulator;
                accumulator = or_stack[stack_ptr - 1];
                stack_ptr--;
            }
            continue;
        }

        if (pc + 1 >= s_bytecode_length) break; // Safety check for operand

        if (opcode >= OP_ADD && opcode <= OP_DIV) {
            uint16_t tag_a_id = (s_bytecode[pc] << 8) | s_bytecode[pc + 1];
            pc += 2;
            if (pc + 4 >= s_bytecode_length) break;
            uint8_t flags_b = s_bytecode[pc];
            uint16_t operand_b_raw = (s_bytecode[pc + 1] << 8) | s_bytecode[pc + 2];
            uint16_t dest_tag_id = (s_bytecode[pc + 3] << 8) | s_bytecode[pc + 4];
            pc += 5;

            if (accumulator) {
                int32_t valA = 0;
                int32_t valB = 0;
                state_get_int(tag_a_id, &valA);
                if (flags_b == 0x00) {
                    state_get_int(operand_b_raw, &valB);
                } else if (flags_b == 0x01) {
                    valB = (int16_t)operand_b_raw;
                }

                int64_t res64 = 0;
                if (opcode == OP_ADD) {
                    res64 = (int64_t)valA + (int64_t)valB;
                } else if (opcode == OP_SUB) {
                    res64 = (int64_t)valA - (int64_t)valB;
                } else if (opcode == OP_MUL) {
                    res64 = (int64_t)valA * (int64_t)valB;
                } else if (opcode == OP_DIV) {
                    if (valB == 0) {
                        res64 = 0;
                    } else {
                        res64 = (int64_t)valA / (int64_t)valB;
                    }
                }

                if (res64 > 2147483647LL) res64 = 2147483647LL;
                if (res64 < -2147483648LL) res64 = -2147483648LL;

                state_set_int(dest_tag_id, (int32_t)res64);
            }
            continue;
        }

        uint16_t tag_id = (s_bytecode[pc] << 8) | s_bytecode[pc + 1];
        pc += 2;

        int32_t val = 0;
        state_get_int(tag_id, &val);
        bool tag_val = (val > 0);

        switch (opcode) {
            case OP_LD:   accumulator = tag_val; break;
            case OP_LDN:  accumulator = !tag_val; break;
            case OP_AND:  accumulator = accumulator && tag_val; break;
            case OP_ANDN: accumulator = accumulator && !tag_val; break;
            case OP_OR:   accumulator = accumulator || tag_val; break;
            case OP_ORN:  accumulator = accumulator || !tag_val; break;
            case OP_ST:   state_set_int(tag_id, accumulator ? 1 : 0); break;
            case OP_SET:  if (accumulator) state_set_int(tag_id, 1); break;
            case OP_RST:  
                if (accumulator) {
                    state_set_int(tag_id, 0);
                    reset_timer_or_counter(tag_id);
                }
                break;

            case OP_TON: {
                if (pc + 1 >= s_bytecode_length) break;
                uint16_t preset_ms = (s_bytecode[pc] << 8) | s_bytecode[pc + 1];
                pc += 2;

                timer_state_t *t = get_or_create_timer(tag_id);
                if (!t) break;

                if (accumulator) {
                    if (!t->active) {
                        t->active = true;
                        t->start_time_ms = now_ms;
                    }
                    int64_t elapsed = now_ms - t->start_time_ms;
                    bool done = (elapsed >= preset_ms);
                    state_set_int(tag_id, done ? 1 : 0);
                    accumulator = done;
                } else {
                    t->active = false;
                    state_set_int(tag_id, 0);
                    accumulator = false;
                }
                break;
            }

            case OP_TOF: {
                if (pc + 1 >= s_bytecode_length) break;
                uint16_t preset_ms = (s_bytecode[pc] << 8) | s_bytecode[pc + 1];
                pc += 2;

                timer_state_t *t = get_or_create_timer(tag_id);
                if (!t) break;

                if (accumulator) {
                    t->active = false;
                    state_set_int(tag_id, 1);
                    accumulator = true;
                } else {
                    if (t->last_acc && !accumulator) {
                        t->active = true;
                        t->start_time_ms = now_ms;
                    }
                    if (t->active) {
                        int64_t elapsed = now_ms - t->start_time_ms;
                        if (elapsed >= preset_ms) {
                            t->active = false;
                            state_set_int(tag_id, 0);
                            accumulator = false;
                        } else {
                            state_set_int(tag_id, 1);
                            accumulator = true;
                        }
                    } else {
                        state_set_int(tag_id, 0);
                        accumulator = false;
                    }
                }
                t->last_acc = accumulator;
                break;
            }

            case OP_CTU: {
                if (pc + 1 >= s_bytecode_length) break;
                uint16_t preset_cnt = (s_bytecode[pc] << 8) | s_bytecode[pc + 1];
                pc += 2;

                counter_state_t *c = get_or_create_counter(tag_id);
                if (!c) break;

                if (accumulator && !c->last_acc) {
                    c->current_count++;
                }
                c->last_acc = accumulator;

                bool done = (c->current_count >= preset_cnt);
                state_set_int(tag_id, done ? 1 : 0);
                accumulator = done;
                break;
            }

            case OP_CTD: {
                if (pc + 1 >= s_bytecode_length) break;
                uint16_t preset_cnt = (s_bytecode[pc] << 8) | s_bytecode[pc + 1];
                pc += 2;

                counter_state_t *c = get_or_create_counter(tag_id);
                if (!c) break;

                if (c->current_count == 0) {
                    c->current_count = preset_cnt;
                }

                if (accumulator && !c->last_acc) {
                    if (c->current_count > 0) c->current_count--;
                }
                c->last_acc = accumulator;

                bool done = (c->current_count == 0);
                state_set_int(tag_id, done ? 1 : 0);
                accumulator = done;
                break;
            }

            case OP_EQU: accumulator = accumulator && (val == 0); break;
            case OP_GRT: accumulator = accumulator && (val > 0); break;
            case OP_LES: accumulator = accumulator && (val < 0); break;

            default: break;
        }
    }
}

bool ladder_engine_run_self_test(void)
{
    ESP_LOGI(TAG, "Iniciando self-test unitario do Ladder Engine (Contrato X/Y)...");

    uint8_t saved_bytecode[LADDER_MAX_PROGRAM_SIZE];
    size_t saved_length = s_bytecode_length;
    bool saved_has_program = s_has_program;
    if (saved_has_program && saved_length > 0) {
        memcpy(saved_bytecode, s_bytecode, saved_length);
    }

    uint8_t test_prog[] = {
        'E', 'N', 'D', 'P', 0x01,
        OP_LD, (ENDAP_INPUT_ID(0) >> 8) & 0xFF, ENDAP_INPUT_ID(0) & 0xFF,
        OP_ST, (ENDAP_OUTPUT_ID(0) >> 8) & 0xFF, ENDAP_OUTPUT_ID(0) & 0xFF,
        0xFF
    };

    esp_err_t err = ladder_engine_load_program(test_prog, sizeof(test_prog));
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Self-test falhou ao carregar programa de teste: %d", err);
        goto fail;
    }

    state_set_int(ENDAP_INPUT_ID(0), 0);
    state_set_int(ENDAP_OUTPUT_ID(0), 1);
    ladder_engine_run();

    int32_t out_val = -1;
    state_get_int(ENDAP_OUTPUT_ID(0), &out_val);
    if (out_val != 0) {
        ESP_LOGE(TAG, "Self-test falhou: X0=0 deveria produzir Y0=0, mas obteve %" PRId32, out_val);
        goto fail;
    }

    state_set_int(ENDAP_INPUT_ID(0), 1);
    ladder_engine_run();

    state_get_int(ENDAP_OUTPUT_ID(0), &out_val);
    if (out_val != 1) {
        ESP_LOGE(TAG, "Self-test falhou: X0=1 deveria produzir Y0=1, mas obteve %" PRId32, out_val);
        goto fail;
    }

    state_set_int(ENDAP_INPUT_ID(0), 0);
    ladder_engine_run();

    state_get_int(ENDAP_OUTPUT_ID(0), &out_val);
    if (out_val != 0) {
        ESP_LOGE(TAG, "Self-test falhou: X0=0 (reset) deveria produzir Y0=0, mas obteve %" PRId32, out_val);
        goto fail;
    }

    if (saved_has_program && saved_length > 0) {
        memcpy(s_bytecode, saved_bytecode, saved_length);
        s_bytecode_length = saved_length;
        s_has_program = true;
    } else {
        ladder_engine_clear();
    }

    ESP_LOGI(TAG, "Self-test unitario do Ladder Engine (Contrato X/Y) concluido com SUCESSO!");
    return true;

fail:
    if (saved_has_program && saved_length > 0) {
        memcpy(s_bytecode, saved_bytecode, saved_length);
        s_bytecode_length = saved_length;
        s_has_program = true;
    } else {
        ladder_engine_clear();
    }
    return false;
}

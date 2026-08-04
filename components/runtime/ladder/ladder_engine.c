#include "ladder_engine.h"
#include <string.h>
#include "esp_log.h"
#include "state.h"
#include "esp_attr.h"
#include "nvs_flash.h"
#include "nvs.h"

static const char *TAG = "ladder";

#define LADDER_MAX_PROGRAM_SIZE 2048

static uint8_t s_bytecode[LADDER_MAX_PROGRAM_SIZE];
static size_t s_bytecode_length = 0;
static bool s_has_program = false;

// OPCODES correspondentes ao compilador do Studio
#define OP_LD    0x01
#define OP_LDN   0x02
#define OP_AND   0x03
#define OP_ANDN  0x04
#define OP_OR    0x05
#define OP_ORN   0x06
#define OP_ST    0x10
#define OP_SET   0x11
#define OP_RST   0x12

// Hash function to resolve Tag IDs
// Em uma implementação real, usaríamos um dicionário para traduzir o HASH_ID no ID físico,
// Mas assumindo que as IOs também possuem IDs 1..100
// bool get_tag_val(uint16_t tag_hash) { ... }
// Para fins da v1 minimalista, se o ID da tag no Studio for "1" (IN1), o hash não bate.
// Nós vamos expor state_get_int/bool diretamente.

esp_err_t ladder_engine_init(void)
{
    s_bytecode_length = 0;
    s_has_program = false;

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

// Rodar no HOT PATH - IRAM_ATTR para garantir determinismo
void IRAM_ATTR ladder_engine_run(void)
{
    if (!s_has_program || s_bytecode_length < 5) return;

    bool accumulator = false;
    size_t pc = 5; // Pula Header (4 bytes) + Versão (1 byte)

    while (pc < s_bytecode_length) {
        uint8_t opcode = s_bytecode[pc++];
        
        if (opcode == 0xFF) {
            break; // Terminator
        }

        if (pc + 1 >= s_bytecode_length) break; // Safety check

        uint16_t tag_id = (s_bytecode[pc] << 8) | s_bytecode[pc + 1];
        pc += 2;

        // Recupera valor da TAG. Como IN1 é channel_id 1, vamos ignorar o HASH por enquanto 
        // e considerar tag_id = channel_id exato caso seja < 100 para simplificar a prova de conceito.
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
            case OP_RST:  if (accumulator) state_set_int(tag_id, 0); break;
            default: break; // Desconhecido, ignora
        }
    }
}

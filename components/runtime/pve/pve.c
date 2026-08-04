#include "pve.h"
#include "event_bus.h"
#include "esp_timer.h"
#include <string.h>


pve_variable_t pve_variables[MAX_PVE_VARIABLES];

void pve_init_variables(void) {
    // AIN1 Default: 0..4095 raw maps to 0..1000 scaled (0..10.0 bar), decimals=1, unit="bar"
    pve_variables[0] = (pve_variable_t){
        .source_type = PVE_SOURCE_ADC_NATIVE,
        .source_id = 6, // ADC1 Channel 6 (GPIO 34)
        .name = "AIN1",
        .runtime = {
            .raw_value = 0,
            .scaled_value = 0,
            .alarm_state = PVE_ALARM_STATE_OK
        },
        .config = {
            .input_min = 0,
            .input_max = 4095,
            .scaled_min = 0,
            .scaled_max = 1000,
            .decimals = 1,
            .unit = "bar"
        },
        .alarm = {
            .high_limit = 800,
            .low_limit = 200,
            .hysteresis = 50,
            .enabled = 0
        }
    };
    
    pve_variables[1] = (pve_variable_t){
        .source_type = PVE_SOURCE_ADC_NATIVE,
        .source_id = 7, // ADC1 Channel 7 (GPIO 35)
        .name = "AIN2",
        .runtime = {
            .raw_value = 0,
            .scaled_value = 0,
            .alarm_state = PVE_ALARM_STATE_OK
        },
        .config = {
            .input_min = 0,
            .input_max = 4095,
            .scaled_min = 0,
            .scaled_max = 10000,
            .decimals = 2,
            .unit = "%"
        },
        .alarm = {
            .high_limit = 9000,
            .low_limit = 1000,
            .hysteresis = 500,
            .enabled = 0
        }
    };

    // AHT10 Temp (Variable 2): Raw/Scaled in 0.1 C
    pve_variables[2] = (pve_variable_t){
        .source_type = PVE_SOURCE_VIRTUAL,
        .source_id = 14,
        .name = "TEMP_AHT",
        .runtime = { .raw_value = 0, .scaled_value = 0, .alarm_state = PVE_ALARM_STATE_OK },
        .config = { .input_min = -400, .input_max = 800, .scaled_min = -400, .scaled_max = 800, .decimals = 1, .unit = "C" },
        .alarm = { .high_limit = 500, .low_limit = 0, .hysteresis = 20, .enabled = 0 }
    };

    // AHT10 Humi (Variable 3): Raw/Scaled in 0.1 %
    pve_variables[3] = (pve_variable_t){
        .source_type = PVE_SOURCE_VIRTUAL,
        .source_id = 15,
        .name = "HUM_AHT",
        .runtime = { .raw_value = 0, .scaled_value = 0, .alarm_state = PVE_ALARM_STATE_OK },
        .config = { .input_min = 0, .input_max = 1000, .scaled_min = 0, .scaled_max = 1000, .decimals = 1, .unit = "%" },
        .alarm = { .high_limit = 800, .low_limit = 200, .hysteresis = 50, .enabled = 0 }
    };

    // DS18B20 Temp (Variable 4): Raw/Scaled in 0.1 C
    pve_variables[4] = (pve_variable_t){
        .source_type = PVE_SOURCE_VIRTUAL,
        .source_id = 16,
        .name = "TEMP_DS",
        .runtime = { .raw_value = 0, .scaled_value = 0, .alarm_state = PVE_ALARM_STATE_OK },
        .config = { .input_min = -550, .input_max = 1250, .scaled_min = -550, .scaled_max = 1250, .decimals = 1, .unit = "C" },
        .alarm = { .high_limit = 600, .low_limit = 50, .hysteresis = 15, .enabled = 0 }
    };

    // DHT11 Temp (Variable 5): Raw/Scaled in 0.1 C
    pve_variables[5] = (pve_variable_t){
        .source_type = PVE_SOURCE_VIRTUAL,
        .source_id = 17,
        .name = "TEMP_DHT",
        .runtime = { .raw_value = 0, .scaled_value = 0, .alarm_state = PVE_ALARM_STATE_OK },
        .config = { .input_min = -200, .input_max = 600, .scaled_min = -200, .scaled_max = 600, .decimals = 1, .unit = "C" },
        .alarm = { .high_limit = 500, .low_limit = 0, .hysteresis = 20, .enabled = 0 }
    };

    // DHT11 Hum (Variable 6): Raw/Scaled in 0.1 %
    pve_variables[6] = (pve_variable_t){
        .source_type = PVE_SOURCE_VIRTUAL,
        .source_id = 18,
        .name = "HUM_DHT",
        .runtime = { .raw_value = 0, .scaled_value = 0, .alarm_state = PVE_ALARM_STATE_OK },
        .config = { .input_min = 0, .input_max = 1000, .scaled_min = 0, .scaled_max = 1000, .decimals = 1, .unit = "%" },
        .alarm = { .high_limit = 800, .low_limit = 200, .hysteresis = 50, .enabled = 0 }
    };
}

const pve_variable_t *pve_get(uint32_t id) {
    if (id >= MAX_PVE_VARIABLES) {
        return NULL;
    }
    return &pve_variables[id];
}

uint32_t pve_count(void) {
    return MAX_PVE_VARIABLES;
}

#define ALARM_HISTORY_MAX 64
static pve_alarm_event_t alarm_history[ALARM_HISTORY_MAX];
static uint32_t alarm_history_head = 0;
static uint32_t alarm_history_count = 0;

static void pve_log_alarm_event(uint16_t pve_id, uint8_t state, int32_t scaled_value) {
    uint32_t now_seconds = (uint32_t)(esp_timer_get_time() / 1000000ULL);
    
    pve_alarm_event_t ev = {
        .timestamp = now_seconds,
        .pve_id = pve_id,
        .state = state,
        .scaled_value = scaled_value
    };
    
    alarm_history[alarm_history_head] = ev;
    alarm_history_head = (alarm_history_head + 1) % ALARM_HISTORY_MAX;
    if (alarm_history_count < ALARM_HISTORY_MAX) {
        alarm_history_count++;
    }
}

uint32_t pve_get_alarm_history(pve_alarm_event_t *out_buffer, uint32_t max_len) {
    if (!out_buffer || max_len == 0) return 0;
    
    uint32_t copy_count = (alarm_history_count < max_len) ? alarm_history_count : max_len;
    
    // Copy from oldest to newest
    uint32_t start_idx = (alarm_history_head - alarm_history_count + ALARM_HISTORY_MAX) % ALARM_HISTORY_MAX;
    for (uint32_t i = 0; i < copy_count; i++) {
        uint32_t idx = (start_idx + i) % ALARM_HISTORY_MAX;
        out_buffer[i] = alarm_history[idx];
    }
    return copy_count;
}

void pve_clear_alarm_history(void) {
    alarm_history_head = 0;
    alarm_history_count = 0;
    memset(alarm_history, 0, sizeof(alarm_history));
}


int32_t pve_scale(
    int32_t raw,
    int32_t input_min,
    int32_t input_max,
    int32_t scaled_min,
    int32_t scaled_max
) {
    if (input_max == input_min) {
        return scaled_min;
    }

    bool is_inverted = (input_max < input_min);
    if (!is_inverted) {
        if (raw <= input_min) {
            return scaled_min;
        }
        if (raw >= input_max) {
            return scaled_max;
        }
    } else {
        if (raw >= input_min) {
            return scaled_min;
        }
        if (raw <= input_max) {
            return scaled_max;
        }
    }

    int64_t numerator = (int64_t)(raw - input_min) * (int64_t)(scaled_max - scaled_min);
    int64_t denominator = (int64_t)(input_max - input_min);

    int64_t result;
    if ((numerator < 0) ^ (denominator < 0)) {
        result = (numerator - denominator / 2) / denominator;
    } else {
        result = (numerator + denominator / 2) / denominator;
    }

    return (int32_t)(scaled_min + result);
}

void pve_update(
    pve_variable_t *var,
    int32_t raw_value
) {
    if (!var) {
        return;
    }
    var->runtime.raw_value = raw_value;
    var->runtime.scaled_value = pve_scale(
        raw_value,
        var->config.input_min,
        var->config.input_max,
        var->config.scaled_min,
        var->config.scaled_max
    );

    uint8_t next_alarm_state = var->runtime.alarm_state;
    if (var->alarm.enabled) {
        if (var->runtime.alarm_state == PVE_ALARM_STATE_HIGH) {
            if (var->runtime.scaled_value <= var->alarm.high_limit - var->alarm.hysteresis) {
                if (var->runtime.scaled_value <= var->alarm.low_limit) {
                    next_alarm_state = PVE_ALARM_STATE_LOW;
                } else {
                    next_alarm_state = PVE_ALARM_STATE_OK;
                }
            }
        } else if (var->runtime.alarm_state == PVE_ALARM_STATE_LOW) {
            if (var->runtime.scaled_value >= var->alarm.low_limit + var->alarm.hysteresis) {
                if (var->runtime.scaled_value >= var->alarm.high_limit) {
                    next_alarm_state = PVE_ALARM_STATE_HIGH;
                } else {
                    next_alarm_state = PVE_ALARM_STATE_OK;
                }
            }
        } else {
            // PVE_ALARM_STATE_OK
            if (var->runtime.scaled_value >= var->alarm.high_limit) {
                next_alarm_state = PVE_ALARM_STATE_HIGH;
            } else if (var->runtime.scaled_value <= var->alarm.low_limit) {
                next_alarm_state = PVE_ALARM_STATE_LOW;
            }
        }
    } else {
        next_alarm_state = PVE_ALARM_STATE_OK;
    }

    if (next_alarm_state != var->runtime.alarm_state) {
        var->runtime.alarm_state = next_alarm_state;

        // Log transition in the circular alarm history
        uint16_t pve_id = (uint16_t)(var - pve_variables);
        pve_log_alarm_event(pve_id, next_alarm_state, var->runtime.scaled_value);

        endap_event_t ev = {
            .source = var->source_id,
            .data = var->runtime.scaled_value
        };

        if (next_alarm_state == PVE_ALARM_STATE_HIGH) {
            ev.type = EVENT_PVE_ALARM_HIGH;
            event_bus_publish(ev);
        } else if (next_alarm_state == PVE_ALARM_STATE_LOW) {
            ev.type = EVENT_PVE_ALARM_LOW;
            event_bus_publish(ev);
        }
    }

}

static const pve_binding_t pve_bindings[] = {
    { .state_id = 12, .pve_id = 0 },
    { .state_id = 13, .pve_id = 1 },
    { .state_id = 14, .pve_id = 2 },
    { .state_id = 15, .pve_id = 3 },
    { .state_id = 16, .pve_id = 4 }
};
#define PVE_BINDINGS_COUNT (sizeof(pve_bindings) / sizeof(pve_bindings[0]))

int32_t pve_get_scaled_value(uint16_t state_id, int32_t fallback) {
    for (size_t i = 0; i < PVE_BINDINGS_COUNT; i++) {
        if (pve_bindings[i].state_id == state_id) {
            uint16_t pve_id = pve_bindings[i].pve_id;
            if (pve_id < MAX_PVE_VARIABLES) {
                return pve_variables[pve_id].runtime.scaled_value;
            }
        }
    }
    return fallback;
}

#include "nvs.h"
#include "esp_log.h"
#include <string.h>
#include "endap_nvs.h"

#define PVE_NAMESPACE "pve"
#define PVE_TAG "PVE"

static uint32_t pve_crc32(const uint8_t *data, size_t len) {
    uint32_t crc = 2166136261u;
    for (size_t i = 0; i < len; i++) {
        crc ^= data[i];
        crc *= 16777619u;
    }
    return crc;
}

typedef struct {
    char name[16];
    int32_t input_min;
    int32_t input_max;
    int32_t scaled_min;
    int32_t scaled_max;
    uint8_t decimals;
    char unit[8];
    uint8_t alarm_enabled;
    int32_t alarm_high;
    int32_t alarm_low;
    int32_t alarm_hysteresis;
    uint32_t crc;
} pve_persisted_config_t;


void pve_save_config(void) {
    nvs_handle_t nvs;
    esp_err_t err = nvs_open(PVE_NAMESPACE, NVS_READWRITE, &nvs);
    if (err != ESP_OK) {
        ESP_LOGE(PVE_TAG, "Erro ao abrir NVS para salvar config: %s", esp_err_to_name(err));
        return;
    }

    for (int i = 0; i < MAX_PVE_VARIABLES; i++) {
        pve_persisted_config_t pc = {0};
        strncpy(pc.name, pve_variables[i].name, sizeof(pc.name) - 1);
        pc.input_min = pve_variables[i].config.input_min;
        pc.input_max = pve_variables[i].config.input_max;
        pc.scaled_min = pve_variables[i].config.scaled_min;
        pc.scaled_max = pve_variables[i].config.scaled_max;
        pc.decimals = pve_variables[i].config.decimals;
        strncpy(pc.unit, pve_variables[i].config.unit, sizeof(pc.unit) - 1);
        pc.alarm_enabled = pve_variables[i].alarm.enabled;
        pc.alarm_high = pve_variables[i].alarm.high_limit;
        pc.alarm_low = pve_variables[i].alarm.low_limit;
        pc.alarm_hysteresis = pve_variables[i].alarm.hysteresis;


        pc.crc = pve_crc32((const uint8_t *)&pc, sizeof(pc) - sizeof(pc.crc));

        char key[16];
        snprintf(key, sizeof(key), "pve_%d", i);

        err = nvs_set_blob(nvs, key, &pc, sizeof(pc));
        if (err != ESP_OK) {
            ESP_LOGE(PVE_TAG, "Erro ao salvar blob para %s: %s", key, esp_err_to_name(err));
        }
    }

    endap_nvs_commit(nvs);
    nvs_close(nvs);
    ESP_LOGI(PVE_TAG, "Configuracoes do PVE salvas com sucesso no NVS");
}

void pve_load_config(void) {
    nvs_handle_t nvs;
    esp_err_t err = nvs_open(PVE_NAMESPACE, NVS_READONLY, &nvs);
    if (err != ESP_OK) {
        ESP_LOGW(PVE_TAG, "Namespace NVS '%s' nao existe ou erro ao abrir. Usando defaults.", PVE_NAMESPACE);
        pve_init_variables();
        return;
    }

    bool loaded_any = false;
    bool any_failed = false;

    for (int i = 0; i < MAX_PVE_VARIABLES; i++) {
        pve_persisted_config_t pc = {0};
        size_t required_size = sizeof(pc);
        char key[16];
        snprintf(key, sizeof(key), "pve_%d", i);

        err = nvs_get_blob(nvs, key, &pc, &required_size);
        if (err == ESP_OK && required_size == sizeof(pc)) {
            uint32_t computed_crc = pve_crc32((const uint8_t *)&pc, sizeof(pc) - sizeof(pc.crc));
            if (computed_crc == pc.crc) {
                strncpy(pve_variables[i].name, pc.name, sizeof(pve_variables[i].name) - 1);
                pve_variables[i].config.input_min = pc.input_min;
                pve_variables[i].config.input_max = pc.input_max;
                pve_variables[i].config.scaled_min = pc.scaled_min;
                pve_variables[i].config.scaled_max = pc.scaled_max;
                pve_variables[i].config.decimals = pc.decimals;
                strncpy(pve_variables[i].config.unit, pc.unit, sizeof(pve_variables[i].config.unit) - 1);
                pve_variables[i].alarm.enabled = pc.alarm_enabled;
                pve_variables[i].alarm.high_limit = pc.alarm_high;
                pve_variables[i].alarm.low_limit = pc.alarm_low;
                pve_variables[i].alarm.hysteresis = pc.alarm_hysteresis;


                // Re-calculate scaled value
                pve_update(&pve_variables[i], pve_variables[i].runtime.raw_value);
                loaded_any = true;
            } else {
                ESP_LOGE(PVE_TAG, "Falha de CRC para %s. Restaurando default.", key);
                any_failed = true;
            }
        } else {
            ESP_LOGW(PVE_TAG, "Nao foi possivel carregar blob para %s. Restaurando default.", key);
            any_failed = true;
        }
    }

    nvs_close(nvs);

    if (any_failed || !loaded_any) {
        ESP_LOGW(PVE_TAG, "Algum canal falhou no carregamento ou NVS vazio. Restaurando defaults de fabrica.");
        pve_init_variables();
    } else {
        ESP_LOGI(PVE_TAG, "Configuracoes do PVE carregadas com sucesso do NVS");
    }
}

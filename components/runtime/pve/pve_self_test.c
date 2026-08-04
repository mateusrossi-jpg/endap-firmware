#include "pve.h"
#include "pve_self_test.h"
#include "esp_log.h"
#include <string.h>

#define TAG "PVE_TEST"

#define TEST_ASSERT(cond, msg) \
    do { \
        if (!(cond)) { \
            ESP_LOGE(TAG, "Test failed at %s:%d: %s", __FILE__, __LINE__, msg); \
            return false; \
        } \
    } while (0)

bool pve_run_self_tests(void) {
    ESP_LOGI(TAG, "Iniciando testes unitários do PVE...");

    // Test 1: Escala crescente (0..4095 -> 0..1000)
    int32_t t1_mid = pve_scale(2048, 0, 4095, 0, 1000);
    TEST_ASSERT(t1_mid == 500, "Falha no Teste 1: Ponto médio de escala crescente (2048 -> 500)");
    
    int32_t t1_quarter = pve_scale(1024, 0, 4095, 0, 1000);
    TEST_ASSERT(t1_quarter == 250, "Falha no Teste 1: Ponto de 25% da escala crescente (1024 -> 250)");

    // Test 2: Escala invertida (0..4095 -> 1000..0)
    int32_t t2_mid = pve_scale(2048, 0, 4095, 1000, 0);
    TEST_ASSERT(t2_mid == 500, "Falha no Teste 2: Ponto médio de escala invertida (2048 -> 500)");
    
    int32_t t2_quarter = pve_scale(1024, 0, 4095, 1000, 0);
    TEST_ASSERT(t2_quarter == 750, "Falha no Teste 2: Ponto de 25% de escala invertida (1024 -> 750)");

    // Test 3: Saturação inferior (raw abaixo de input_min)
    int32_t t3_sat = pve_scale(-100, 0, 4095, 0, 1000);
    TEST_ASSERT(t3_sat == 0, "Falha no Teste 3: Saturação inferior em escala crescente (-100 -> 0)");
    
    int32_t t3_sat_inv = pve_scale(4200, 4095, 0, 0, 1000);
    TEST_ASSERT(t3_sat_inv == 0, "Falha no Teste 3: Saturação inferior em escala invertida de entrada (4200 -> 0)");

    // Test 4: Saturação superior (raw acima de input_max)
    int32_t t4_sat = pve_scale(5000, 0, 4095, 0, 1000);
    TEST_ASSERT(t4_sat == 1000, "Falha no Teste 4: Saturação superior em escala crescente (5000 -> 1000)");

    int32_t t4_sat_inv = pve_scale(-50, 4095, 0, 0, 1000);
    TEST_ASSERT(t4_sat_inv == 1000, "Falha no Teste 4: Saturação superior em escala invertida de entrada (-50 -> 1000)");

    // Test 5: Faixa percentual (0..4095 -> 0..10000)
    int32_t t5_val = pve_scale(2048, 0, 4095, 0, 10000);
    TEST_ASSERT(t5_val == 5001, "Falha no Teste 5: Ponto médio da faixa percentual (2048 -> 5001)");

    // Test 6: Faixa industrial (4-20mA representada em bruto 819..4095 escalada para 0..10000)
    int32_t t6_min = pve_scale(819, 819, 4095, 0, 10000);
    TEST_ASSERT(t6_min == 0, "Falha no Teste 6: Limite mínimo de 4mA (819 -> 0)");
    
    int32_t t6_mid = pve_scale(2457, 819, 4095, 0, 10000);
    TEST_ASSERT(t6_mid == 5000, "Falha no Teste 6: Ponto médio de 12mA (2457 -> 5000)");
    
    int32_t t6_max = pve_scale(4095, 819, 4095, 0, 10000);
    TEST_ASSERT(t6_max == 10000, "Falha no Teste 6: Limite máximo de 20mA (4095 -> 10000)");

    // Test 7: Proteção contra input_max == input_min
    int32_t t7_val = pve_scale(2000, 1000, 1000, 50, 100);
    TEST_ASSERT(t7_val == 50, "Falha no Teste 7: Divisão por zero/faixa nula (2000 -> 50)");

    // Test 8: Teste de estabilidade matemática (1000 execuções consecutivas dando o mesmo resultado)
    int32_t first_result = pve_scale(1500, 0, 4095, 0, 1000);
    for (int i = 0; i < 1000; i++) {
        int32_t iter_result = pve_scale(1500, 0, 4095, 0, 1000);
        TEST_ASSERT(iter_result == first_result, "Falha no Teste 8: Instabilidade matemática encontrada");
    }

    // Test 9: Teste de pve_get_scaled_value
    pve_init_variables();
    pve_variables[0].runtime.scaled_value = 598;
    pve_variables[1].runtime.scaled_value = 7500;

    int32_t test_val_12 = pve_get_scaled_value(12, 100);
    TEST_ASSERT(test_val_12 == 598, "Falha no Teste 9: state_id 12 deve retornar scaled_value de pve_variables[0]");

    int32_t test_val_13 = pve_get_scaled_value(13, 200);
    TEST_ASSERT(test_val_13 == 7500, "Falha no Teste 9: state_id 13 deve retornar scaled_value de pve_variables[1]");

    int32_t test_val_other = pve_get_scaled_value(10, 42);
    TEST_ASSERT(test_val_other == 42, "Falha no Teste 9: outro state_id deve retornar o fallback");

    // Test 10: Teste de persistência de configuração (save & load)
    pve_variable_t backup_vars[MAX_PVE_VARIABLES];
    memcpy(backup_vars, pve_variables, sizeof(backup_vars));

    strncpy(pve_variables[0].name, "TEST_P1", sizeof(pve_variables[0].name) - 1);
    pve_variables[0].config.input_min = 100;
    pve_variables[0].config.input_max = 900;
    pve_variables[0].config.scaled_min = 10;
    pve_variables[0].config.scaled_max = 90;
    pve_variables[0].config.decimals = 3;
    strncpy(pve_variables[0].config.unit, "psi", sizeof(pve_variables[0].config.unit) - 1);
    pve_variables[0].alarm.enabled = 1;
    pve_variables[0].alarm.high_limit = 85;
    pve_variables[0].alarm.low_limit = 15;

    strncpy(pve_variables[1].name, "TEST_P2", sizeof(pve_variables[1].name) - 1);
    pve_variables[1].config.input_min = 200;
    pve_variables[1].config.input_max = 800;
    pve_variables[1].config.scaled_min = 20;
    pve_variables[1].config.scaled_max = 80;
    pve_variables[1].config.decimals = 0;
    strncpy(pve_variables[1].config.unit, "C", sizeof(pve_variables[1].config.unit) - 1);
    pve_variables[1].alarm.enabled = 0;
    pve_variables[1].alarm.high_limit = 70;
    pve_variables[1].alarm.low_limit = 30;

    pve_save_config();

    pve_init_variables();

    pve_load_config();

    TEST_ASSERT(strcmp(pve_variables[0].name, "TEST_P1") == 0, "Falha no Teste 10: nome de pve[0] incorreto");
    TEST_ASSERT(pve_variables[0].config.input_min == 100, "Falha no Teste 10: input_min de pve[0] incorreto");
    TEST_ASSERT(pve_variables[0].config.input_max == 900, "Falha no Teste 10: input_max de pve[0] incorreto");
    TEST_ASSERT(pve_variables[0].config.scaled_min == 10, "Falha no Teste 10: scaled_min de pve[0] incorreto");
    TEST_ASSERT(pve_variables[0].config.scaled_max == 90, "Falha no Teste 10: scaled_max de pve[0] incorreto");
    TEST_ASSERT(pve_variables[0].config.decimals == 3, "Falha no Teste 10: decimals de pve[0] incorreto");
    TEST_ASSERT(strcmp(pve_variables[0].config.unit, "psi") == 0, "Falha no Teste 10: unit de pve[0] incorreto");
    TEST_ASSERT(pve_variables[0].alarm.enabled == 1, "Falha no Teste 10: alarm_enabled de pve[0] incorreto");
    TEST_ASSERT(pve_variables[0].alarm.high_limit == 85, "Falha no Teste 10: alarm_high de pve[0] incorreto");
    TEST_ASSERT(pve_variables[0].alarm.low_limit == 15, "Falha no Teste 10: alarm_low de pve[0] incorreto");

    TEST_ASSERT(strcmp(pve_variables[1].name, "TEST_P2") == 0, "Falha no Teste 10: nome de pve[1] incorreto");
    TEST_ASSERT(pve_variables[1].config.input_min == 200, "Falha no Teste 10: input_min de pve[1] incorreto");
    TEST_ASSERT(pve_variables[1].config.input_max == 800, "Falha no Teste 10: input_max de pve[1] incorreto");
    TEST_ASSERT(pve_variables[1].config.scaled_min == 20, "Falha no Teste 10: scaled_min de pve[1] incorreto");
    TEST_ASSERT(pve_variables[1].config.scaled_max == 80, "Falha no Teste 10: scaled_max de pve[1] incorreto");
    TEST_ASSERT(pve_variables[1].config.decimals == 0, "Falha no Teste 10: decimals de pve[1] incorreto");
    TEST_ASSERT(strcmp(pve_variables[1].config.unit, "C") == 0, "Falha no Teste 10: unit de pve[1] incorreto");
    TEST_ASSERT(pve_variables[1].alarm.enabled == 0, "Falha no Teste 10: alarm_enabled de pve[1] incorreto");
    TEST_ASSERT(pve_variables[1].alarm.high_limit == 70, "Falha no Teste 10: alarm_high de pve[1] incorreto");
    TEST_ASSERT(pve_variables[1].alarm.low_limit == 30, "Falha no Teste 10: alarm_low de pve[1] incorreto");
    // Test 11: Hysteresis and Alarm History test
    pve_init_variables();
    pve_variables[0].alarm.enabled = 1;
    pve_variables[0].alarm.high_limit = 800;
    pve_variables[0].alarm.low_limit = 200;
    pve_variables[0].alarm.hysteresis = 50;
    pve_variables[0].runtime.alarm_state = PVE_ALARM_STATE_OK;
    
    // Disparo HIGH: scaled_value = 800 (>= 800)
    pve_update(&pve_variables[0], 3276); // maps 3276 -> scaled 800
    TEST_ASSERT(pve_variables[0].runtime.alarm_state == PVE_ALARM_STATE_HIGH, "Falha no Teste 11: limite high nao disparou");
    
    // Retorno para normal com 750 (800 - 50 = 750)
    // Se scaled_value = 751, deve continuar HIGH devido à histerese
    pve_update(&pve_variables[0], 3075); // maps 3075 -> scaled 751
    TEST_ASSERT(pve_variables[0].runtime.alarm_state == PVE_ALARM_STATE_HIGH, "Falha no Teste 11: histerese nao segurou o alarme HIGH em 751");
    
    // Se scaled_value = 750, deve sair de HIGH para OK
    pve_update(&pve_variables[0], 3071); // maps 3071 -> scaled 750
    TEST_ASSERT(pve_variables[0].runtime.alarm_state == PVE_ALARM_STATE_OK, "Falha no Teste 11: alarme nao normalizou em 750");

    // Verificar histórico de alarmes
    pve_alarm_event_t events[10];
    uint32_t count_ev = pve_get_alarm_history(events, 10);
    TEST_ASSERT(count_ev >= 2, "Falha no Teste 11: historico de alarmes devia conter pelo menos 2 eventos");
    TEST_ASSERT(events[count_ev - 2].state == PVE_ALARM_STATE_HIGH, "Falha no Teste 11: primeiro evento devia ser HIGH");
    TEST_ASSERT(events[count_ev - 1].state == PVE_ALARM_STATE_OK, "Falha no Teste 11: segundo evento devia ser OK");

    // Auditoria 1 — Overflow do histórico (Ring Buffer de 64 elementos, gerar 200 eventos)
    pve_init_variables();
    pve_variables[0].alarm.enabled = 1;
    pve_variables[0].alarm.high_limit = 800;
    pve_variables[0].alarm.low_limit = 200;
    pve_variables[0].alarm.hysteresis = 50;
    pve_variables[0].runtime.alarm_state = PVE_ALARM_STATE_OK;

    for (int i = 0; i < 100; i++) {
        pve_update(&pve_variables[0], 3276); // maps to 800 -> HIGH
        pve_update(&pve_variables[0], 3071); // maps to 750 -> OK
    }

    pve_alarm_event_t audit_events[100];
    uint32_t audit_count = pve_get_alarm_history(audit_events, 100);
    TEST_ASSERT(audit_count == 64, "Auditoria 1 Falhou: buffer circular nao limitou a 64 eventos");
    TEST_ASSERT(audit_events[63].state == PVE_ALARM_STATE_OK, "Auditoria 1 Falhou: ultimo evento devia ser OK");
    TEST_ASSERT(audit_events[62].state == PVE_ALARM_STATE_HIGH, "Auditoria 1 Falhou: penultimo evento devia ser HIGH");

    // Auditoria 2 — Tempestade de alarmes (Chatter)
    pve_clear_alarm_history();
    uint32_t count_before_storm = pve_get_alarm_history(audit_events, 64);
    
    // Normalizar primeiro
    pve_variables[0].runtime.alarm_state = PVE_ALARM_STATE_HIGH;
    pve_update(&pve_variables[0], 3071); // maps to 750 -> OK
    
    // Aplicar oscilações rápidas (799 < limit, 801 > limit)
    pve_update(&pve_variables[0], 3272); // maps to 799 -> continua OK
    pve_update(&pve_variables[0], 3280); // maps to 801 -> HIGH (Dispara!)
    pve_update(&pve_variables[0], 3272); // maps to 799 -> continua HIGH (devido à histerese de 50)
    pve_update(&pve_variables[0], 3280); // maps to 801 -> continua HIGH
    pve_update(&pve_variables[0], 3272); // maps to 799 -> continua HIGH
    pve_update(&pve_variables[0], 3280); // maps to 801 -> continua HIGH
    
    uint32_t count_after_storm = pve_get_alarm_history(audit_events, 64);
    TEST_ASSERT(count_after_storm - count_before_storm == 2, "Auditoria 2 Falhou: gerou tempestade de alarmes (chatter registrado)");

    memcpy(pve_variables, backup_vars, sizeof(pve_variables));
    pve_save_config();


    ESP_LOGI(TAG, "Todos os testes unitários do PVE passaram com sucesso!");
    return true;
}

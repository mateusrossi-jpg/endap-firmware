#include "io_mapping_persistence.h"

#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "esp_littlefs.h"
#include "cJSON.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static const char *TAG = "IO_MAP_PERSIST";

#define IO_MAPPING_FILE_PATH "/littlefs/io_mapping.json"
#define IO_MAPPING_TMP_PATH  "/littlefs/io_mapping.json.tmp"

static SemaphoreHandle_t s_io_mapping_mutex = NULL;

esp_err_t io_mapping_persistence_init(void)
{
    if (s_io_mapping_mutex == NULL) {
        s_io_mapping_mutex = xSemaphoreCreateMutex();
        if (s_io_mapping_mutex == NULL) {
            ESP_LOGE(TAG, "Falha critica: nao foi possivel criar o Mutex de I/O Mapping no boot");
            return ESP_ERR_NO_MEM;
        }

        // Configuração do VFS LittleFS apontando para a partição "storage" do partitions.csv
        esp_vfs_littlefs_conf_t conf = {
            .base_path = "/littlefs",
            .partition_label = "storage",

            // POLITICA DE FORMATACAO: True em Dev/Bring-up (partição virgem); False em Produção V1.
#if defined(CONFIG_ENDAP_PRODUCTION_BUILD)
            .format_if_mount_failed = false, // Proibido reformatar em campo para evitar perda silenciosa de configs
#else
            .format_if_mount_failed = true,  // Permitido auto-format em desenvolvimento na primeira inicializacao
#endif
            .dont_mount = false,
        };

        esp_err_t ret = esp_vfs_littlefs_register(&conf);
        if (ret != ESP_OK) {
            if (ret == ESP_FAIL) {
                ESP_LOGE(TAG, "[ALERTA CRITICO DE CAMPO] Falha ao montar particao LittleFS (storage). "
                              "Auto-formatacao desativada para preservar dados. Requer intervencao de diagnostico.");
            } else if (ret == ESP_ERR_NOT_FOUND) {
                ESP_LOGE(TAG, "Particao 'storage' com subtype 0x83 nao encontrada no partitions.csv");
            } else {
                ESP_LOGE(TAG, "Erro ao registrar LittleFS VFS (%s)", esp_err_to_name(ret));
            }
            return ret;
        }

        ESP_LOGI(TAG, "LittleFS (/littlefs) montado e Mutex de I/O Mapping inicializado no boot");
    }
    return ESP_OK;
}

esp_err_t io_mapping_save_atomic(const char *json_payload, size_t len)
{
    if (json_payload == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    // =========================================================================
    // PASSO 1: VALIDAÇÃO EXPLÍCITA DE TAMANHO (Fora do lock, evita consumo de RAM/Heap)
    // =========================================================================
    if (len == 0 || len > MAX_IO_MAPPING_SIZE) {
        ESP_LOGE(TAG, "[HTTP 400] Payload rejeitado: tamanho (%zu B) excede o limite MAX_IO_MAPPING_SIZE (%d B)",
                 len, MAX_IO_MAPPING_SIZE);
        return ESP_ERR_INVALID_SIZE; // Mapeado para HTTP 400 Bad Request
    }

    // Garantia de que o mutex e VFS foram inicializados no boot
    if (s_io_mapping_mutex == NULL) {
        ESP_LOGE(TAG, "[HTTP 500] Erro interno: io_mapping_persistence_init() nao foi chamado no boot");
        return ESP_ERR_INVALID_STATE;
    }

    // =========================================================================
    // PASSO 2: AQUISIÇÃO DO MUTEX COM TIMEOUT EXPLÍCITO (2000 ms)
    // =========================================================================
    if (xSemaphoreTake(s_io_mapping_mutex, pdMS_TO_TICKS(IO_MAPPING_MUTEX_TIMEOUT_MS)) != pdTRUE) {
        ESP_LOGE(TAG, "[HTTP 503] Timeout (%d ms) ao adquirir mutex de gravação de I/O Mapping. Task HTTPD liberada.",
                 IO_MAPPING_MUTEX_TIMEOUT_MS);
        return ESP_ERR_TIMEOUT; // Mapeado para HTTP 503 Service Unavailable
    }

    // =========================================================================
    // PASSO 3: VALIDAÇÃO DE SINTAXE JSON (cJSON) COM MUTEX ADQUIRIDO
    // =========================================================================
    cJSON *root = cJSON_ParseWithLength(json_payload, len);
    if (root == NULL) {
        ESP_LOGE(TAG, "[HTTP 400] Falha no parse do JSON (Sintaxe invalida ou JSON corrompido)");
        xSemaphoreGive(s_io_mapping_mutex); // Release obrigatorio antes de sair
        return ESP_ERR_INVALID_ARG; // Mapeado para HTTP 400 Bad Request
    }
    cJSON_Delete(root); // Liberar arvore do heap imediatamente apos validar

    // Inicio da medicao de benchmark do stall de gravação
    int64_t t_start = esp_timer_get_time();

    // =========================================================================
    // PASSO 4: ESCRITA NO ARQUIVO TEMPORÁRIO (.tmp)
    // =========================================================================
    FILE *f = fopen(IO_MAPPING_TMP_PATH, "w");
    if (f == NULL) {
        ESP_LOGE(TAG, "[HTTP 500] Falha ao criar arquivo temporario: %s", IO_MAPPING_TMP_PATH);
        xSemaphoreGive(s_io_mapping_mutex);
        return ESP_FAIL; // Mapeado para HTTP 500 Internal Error
    }

    size_t written = fwrite(json_payload, 1, len, f);

    // =========================================================================
    // PASSO 5: FLUSH DE BUFFER DA C & SINCRONIZAÇÃO DE HARDWARE (fsync)
    // =========================================================================
    fflush(f);
    fsync(fileno(f));
    fclose(f);

    if (written != len) {
        ESP_LOGE(TAG, "[HTTP 500] Escrita incompleta no .tmp (%zu / %zu B). Abortando.", written, len);
        unlink(IO_MAPPING_TMP_PATH);
        xSemaphoreGive(s_io_mapping_mutex);
        return ESP_FAIL;
    }

    // =========================================================================
    // PASSO 6: RENAME ATÔMICO SAFE NO LITTLEFS (.tmp -> .json)
    // =========================================================================
    if (rename(IO_MAPPING_TMP_PATH, IO_MAPPING_FILE_PATH) != 0) {
        ESP_LOGE(TAG, "[HTTP 500] Falha no rename de %s para %s", IO_MAPPING_TMP_PATH, IO_MAPPING_FILE_PATH);
        unlink(IO_MAPPING_TMP_PATH);
        xSemaphoreGive(s_io_mapping_mutex);
        return ESP_FAIL;
    }

    int64_t t_end = esp_timer_get_time();
    uint32_t duration_ms = (uint32_t)((t_end - t_start) / 1000);

    ESP_LOGI(TAG, "I/O Mapping salvo com sucesso (%zu B em %lu ms) -> Escrita atomica LittleFS concluida",
             len, (unsigned long)duration_ms);

    // =========================================================================
    // PASSO 7: LIBERAÇÃO OBRIGATÓRIA DO MUTEX
    // =========================================================================
    xSemaphoreGive(s_io_mapping_mutex);
    return ESP_OK; // HTTP 200 OK
}

#pragma once

#include <esp_err.h>
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Limite maximo do payload JSON de I/O Mapping (32 KB).
 * Protege contra estouro de Heap (OOM) antes do cJSON_Parse.
 */
#define MAX_IO_MAPPING_SIZE (32 * 1024)

/**
 * @brief Timeout maximo de aquisicao do Mutex de Persistencia (2000 ms).
 * Evita travar a task httpd em caso de degradacao de gravação de Flash.
 */
#define IO_MAPPING_MUTEX_TIMEOUT_MS 2000

/**
 * @brief Inicializa o mutex de sincronizacao de persistencia do I/O mapping no boot do sistema.
 * DEVE ser chamado obrigatoriamente durante o boot do ENDAP (ex: core_boot.c).
 * 
 * @return esp_err_t ESP_OK em caso de sucesso.
 */
esp_err_t io_mapping_persistence_init(void);

/**
 * @brief Grava a tabela de I/O Mapping com escrita atomica (.tmp -> rename) no LittleFS.
 * 
 * Ordem estrita das operacoes:
 * 1. Validacao de tamanho (strlen / len > MAX_IO_MAPPING_SIZE)
 * 2. Aquisicao do Mutex (pre-inicializado no boot) com timeout explícito (2000 ms)
 * 3. Validação de sintaxe JSON (cJSON_ParseWithLength)
 * 4. Escrita no arquivo temporario (.tmp)
 * 5. Flush de buffer (fflush) + Sincronizacao de Hardware (fsync)
 * 6. Rename atomico (.tmp -> .json)
 * 7. Liberação obrigatoria do Mutex (xSemaphoreGive)
 * 
 * @param json_payload String contendo o JSON vindo do WebSocket/REST.
 * @param len Tamanho da string do payload.
 * @return esp_err_t ESP_OK (200), ESP_ERR_INVALID_SIZE (400), ESP_ERR_TIMEOUT (503), ESP_FAIL (500).
 */
esp_err_t io_mapping_save_atomic(const char *json_payload, size_t len);

#ifdef __cplusplus
}
#endif

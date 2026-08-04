#ifndef SYSTEM_DIAG_H
#define SYSTEM_DIAG_H

#include <stdint.h>
#include "esp_system.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Inicializa o diagnóstico de sistema no boot:
 *        - Lê o contador de boot da NVS (namespace "system_diag", chave "boot_count")
 *        - Incrementa em 1 e re-salva na NVS (apenas 1x por boot do sistema)
 *        - Registra o motivo do último reset (esp_reset_reason)
 */
void system_diag_init(void);

/**
 * @brief Retorna o número de boots acumulados e persistidos na NVS.
 */
uint32_t system_diag_get_boot_count(void);

/**
 * @brief Retorna a razão do último reset do ESP32.
 */
esp_reset_reason_t system_diag_get_reset_reason(void);

/**
 * @brief Retorna uma string descritiva do motivo do reset.
 */
const char *system_diag_get_reset_reason_str(void);

#ifdef __cplusplus
}
#endif

#endif // SYSTEM_DIAG_H

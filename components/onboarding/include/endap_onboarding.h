#pragma once

#include "device_profile.h"
#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ENDAP_NODE_NAME_MAX 32
#define ENDAP_ONBOARDING_CONFIG_VERSION 1

/**
 * @brief Estados do processo de Onboarding v1.
 */
typedef enum {
    ENDAP_ONBOARDING_STATE_PENDING    = 0,  /*!< Nó virgem/pendente de adoção */
    ENDAP_ONBOARDING_STATE_DISCOVERED = 1,  /*!< Anunciado na rede, detectado pelo Gateway */
    ENDAP_ONBOARDING_STATE_CLAIMED    = 2,  /*!< Adoção em processamento / Claim recebido */
    ENDAP_ONBOARDING_STATE_ACTIVE     = 3,  /*!< Adotado, perfil aplicado e em operação */
} endap_onboarding_state_t;

/**
 * @brief Estrutura de configuração e estado persistido de onboarding.
 */
typedef struct {
    uint8_t version;                       /*!< Versão da struct de configuração */
    uint8_t state;                         /*!< Estado atual (endap_onboarding_state_t) */
    uint8_t profile;                       /*!< Perfil configurado (node_profile_t) */
    uint8_t reserved0;                     /*!< Reservado para alinhamento */
    uint32_t adopted_by_gateway_id;        /*!< ID do Gateway que adotou o nó */
    uint32_t adopted_timestamp;            /*!< Timestamp do momento de adoção */
    char node_name[ENDAP_NODE_NAME_MAX];   /*!< Nome amigável atribuído ao nó */
} endap_onboarding_config_t;

/**
 * @brief Inicializa o serviço de onboarding no boot.
 * 
 * Lê a NVS. Se a chave não existir:
 * - Se for perfil Gateway: marca automaticamente como ACTIVE.
 * - Se for Field/Relay/Sensor: marca como PENDING.
 * 
 * @return esp_err_t ESP_OK em caso de sucesso.
 */
esp_err_t endap_onboarding_init(void);

/**
 * @brief Obtém o estado atual de onboarding do nó.
 * 
 * @return endap_onboarding_state_t Estado atual.
 */
endap_onboarding_state_t endap_onboarding_get_state(void);

/**
 * @brief Verifica se o nó está pendente de onboarding/adoção.
 * 
 * @return true se o estado for PENDING ou DISCOVERED, false caso contrário.
 */
bool endap_onboarding_is_pending(void);

/**
 * @brief Obtém uma cópia do snapshot de configuração de onboarding ativo.
 * 
 * @param out_cfg Ponteiro para receber a struct de configuração.
 * @return esp_err_t ESP_OK em caso de sucesso, ESP_ERR_INVALID_ARG se out_cfg for NULL.
 */
esp_err_t endap_onboarding_get_config(endap_onboarding_config_t *out_cfg);

/**
 * @brief Executa o procedimento de Adoção (Claim) do nó.
 * 
 * Esta função é IDEMPOTENTE. Se chamada com os mesmos parâmetros quando o nó
 * já estiver ACTIVE, retorna ESP_OK sem reaplicar ou regravar desnecessariamente.
 * 
 * Ações executadas:
 * 1. Aplica o template de perfil usando device_profile_apply_template(profile)
 * 2. Atualiza os campos de estado, gateway_id e node_name
 * 3. Persiste a configuração na NVS (namespace "onboarding")
 * 4. Transiciona o estado para ACTIVE
 * 
 * ATENÇÃO: Esta função opera estritamente fora do hot-path.
 * 
 * @param profile Perfil de nó a ser aplicado (node_profile_t)
 * @param gateway_id ID do Gateway adotante (0 se onboarding local)
 * @param node_name Nome amigável do nó (opcional, até ENDAP_NODE_NAME_MAX)
 * @return esp_err_t ESP_OK em caso de sucesso ou código de erro em caso de falha.
 */
esp_err_t endap_onboarding_claim(node_profile_t profile, uint32_t gateway_id, const char *node_name);

/**
 * @brief Reseta o estado do onboarding para o estado inicial PENDING (Factory Reset / Unclaim).
 * 
 * Apaga as chaves NVS do namespace "onboarding" e força o nó de volta para PENDING.
 * 
 * @return esp_err_t ESP_OK em caso de sucesso.
 */
esp_err_t endap_onboarding_reset(void);

/**
 * @brief Realiza um Factory Reset Completo no nó (Onboarding + DeviceProfile + Redes/Transportes).
 * 
 * Limpa o namespace NVS de onboarding, restaura o perfil de nó para o default seguro (Field Node),
 * apaga as configurações salvas de rede e deixa o nó em estado virgem/PENDING.
 * 
 * @return esp_err_t ESP_OK em caso de sucesso.
 */
esp_err_t endap_factory_reset_full(void);

/**
 * @brief Obtém o nome textual amigável do estado informado.
 * 
 * @param state Estado a consultar
 * @return const char* Nome do estado
 */
const char *endap_onboarding_state_name(endap_onboarding_state_t state);

#ifdef __cplusplus
}
#endif

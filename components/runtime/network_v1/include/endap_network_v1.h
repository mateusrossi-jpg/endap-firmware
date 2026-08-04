#pragma once

#include "device_profile.h"
#include "cluster_transport.h"
#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Estados da máquina de conexão de rede da Network v1.
 */
typedef enum {
    ENDAP_NET_STATE_DISCONNECTED = 0, /*!< Desconectado do link primário e fallback */
    ENDAP_NET_STATE_CONNECTING   = 1, /*!< Estabelecendo interface de transporte */
    ENDAP_NET_STATE_CONNECTED    = 2, /*!< Operando no transporte primário */
    ENDAP_NET_STATE_DEGRADED     = 3, /*!< Falha no primário; operando no transporte de fallback */
    ENDAP_NET_STATE_RECOVERING   = 4, /*!< Testando restabelecimento do transporte primário (Hysteresis) */
} endap_network_state_t;

/**
 * @brief Métricas mínimas de saúde e desempenho do transporte de rede.
 */
typedef struct {
    uint32_t packets_sent;         /*!< Total de quadros/heartbeats transmitidos */
    uint32_t packets_received;     /*!< Total de quadros/heartbeats recebidos */
    uint32_t packet_drops;         /*!< Total de falhas de envio/timeouts */
    uint32_t last_rtt_ms;          /*!< Último tempo estimado de RTT */
    uint32_t max_jitter_ms;        /*!< Jitter máximo registrado */
    uint32_t failover_count;       /*!< Contador de transições de failover para o transporte fallback */
    uint8_t active_transport;      /*!< Transporte ativo atual (cluster_transport_type_t) */
    uint8_t current_state;         /*!< Estado atual (endap_network_state_t) */
} endap_network_metrics_t;

/**
 * @brief Inicializa o módulo Network v1 conforme as políticas do NodeProfile ativo.
 * 
 * @return esp_err_t ESP_OK em caso de sucesso.
 */
esp_err_t endap_network_v1_init(void);

/**
 * @brief Processo periódico da máquina de estados e recovery de rede.
 * 
 * ATENÇÃO: Esta função deve rodar EXCLUSIVAMENTE em tasks de baixa prioridade / service.
 * NUNCA chamar no hot-path ou no control_loop.
 */
void endap_network_v1_process(void);

/**
 * @brief Obtém o estado atual da máquina de conexão de rede.
 * 
 * @return endap_network_state_t Estado atual.
 */
endap_network_state_t endap_network_v1_get_state(void);

/**
 * @brief Registra evento de recebimento/envio de pacote para contabilizar métricas leves.
 * 
 * @param is_rx true se for recebimento, false se for envio
 * @param success true se enviado com sucesso, false se falhou/dropou
 * @param rtt_ms tempo de RTT estimado em milissegundos (0 se não aplicável)
 */
void endap_network_v1_note_packet(bool is_rx, bool success, uint32_t rtt_ms);

/**
 * @brief Obtém um snapshot leve das métricas da rede.
 * 
 * @param out_metrics Ponteiro para receber a cópia das métricas.
 */
void endap_network_v1_get_metrics(endap_network_metrics_t *out_metrics);

/**
 * @brief Força uma tentativa manual de reconexão / failover.
 * 
 * @return esp_err_t ESP_OK em caso de sucesso.
 */
esp_err_t endap_network_v1_force_reconnect(void);

/**
 * @brief Retorna o nome amigável do estado da rede.
 * 
 * @param state Estado a consultar
 * @return const char* Nome do estado
 */
const char *endap_network_v1_state_name(endap_network_state_t state);

#ifdef __cplusplus
}
#endif

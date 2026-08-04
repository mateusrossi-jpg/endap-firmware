#ifndef V3_PERSISTENCE_H
#define V3_PERSISTENCE_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

/* O vetor de sujeira alocado no BSS (32 * 32 = 1024 slots de recursos). Gasta exatos 128 Bytes! */
typedef struct {
    uint32_t dirty_mask[32];
} v3_persistence_journal_t;

/* * Notifica a projeção de persistência de que o registo no índice 'catalog_index' foi alterado.
 * Operação estritamente O(1) e não-bloqueante (Bitwise OR).
 */
void v3_persistence_mark_dirty(size_t catalog_index);

/* * O motor de projeção (Deve ser invocado por uma Task de baixa prioridade a cada 5 segundos).
 * Varre o vetor de sujeira, faz a dobradura de chave para 32-bit e consolida os blobs na NVS.
 * Retorna a quantidade de registos fisicamente gravados neste ciclo.
 */
size_t v3_persistence_flush_dirty_records(void);

/* * Restaura a árvore canónica a partir da Flash NVS durante o Boot do Gateway.
 * Deve ser chamada UMA ÚNICA VEZ antes de ligar o GPTimer de 1ms.
 */
bool v3_persistence_restore_catalog(void);

#endif // V3_PERSISTENCE_H

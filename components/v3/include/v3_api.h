#ifndef V3_API_H
#define V3_API_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "v3_identity.h"

/* Tamanho cirúrgico do buffer de stack para streaming NDJSON */
#define V3_API_STREAM_BUFFER_SIZE 512

/* * Contrato de Despejo Agnóstico (Sink Callback)
 * O Servidor HTTP nativo ou a UART injetam a sua própria função de 'write/send'.
 */
typedef size_t (*v3_stream_sink_fn)(void *sink_ctx, const uint8_t *chunk, size_t len);

/* Cursor Absoluto de Paginação (Imune a mutações de deslocamento do array) */
typedef struct {
    resource_id_t after_id; // 0 indica "iniciar do zero absoluto"
    uint16_t      limit;    // Quantidade máxima de linhas NDJSON a cuspir (ex: 20)
} v3_api_cursor_t;

/* * Serializa o Catálogo de Recursos no formato NDJSON (Newline-Delimited JSON)
 * Retorna o ID do último recurso transmitido (para o cliente usar como 'after_id' na próxima requisição).
 * Retorna 0 quando a listagem atinge o fim do catálogo.
 */
resource_id_t v3_api_stream_catalog_ndjson(v3_stream_sink_fn sink, void *sink_ctx, const v3_api_cursor_t *cursor);

#endif // V3_API_H

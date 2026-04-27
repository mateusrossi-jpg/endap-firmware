#ifndef DETERMINISTIC_SNAPSHOT_H
#define DETERMINISTIC_SNAPSHOT_H

/* ============================================================
   DETERMINISTIC SNAPSHOT

   Stub reservado para evolução futura.

   Situação atual:
   - mantido apenas para preservar compatibilidade estrutural
   - não participa ativamente do caminho crítico
   - não possui processamento funcional nesta fase do ENDAP v1

   Diretriz:
   - não expandir este módulo agora
   - só implementar quando existir requisito claro de snapshot
     determinístico no runtime
============================================================ */

void deterministic_snapshot_init(void);
void deterministic_snapshot_tick(void);

#endif

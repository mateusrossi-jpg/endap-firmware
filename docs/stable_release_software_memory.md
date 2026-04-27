# ENDAP Stable Release - Software Memory

Este arquivo registra as decisoes de software que devem guiar o fechamento do
Stable Release. Ele existe para manter continuidade entre sessoes de trabalho.

## Prioridades Congeladas

1. Fail-safe por canal: politica por saida, startup seguro, perda de comunicacao,
   rearme manual e origem efetiva da regra aplicada.
2. Seguranca V2: contas, papeis, sessao estavel, troca de senha e protecao uniforme
   dos endpoints criticos.
3. Admissao de nos: fluxo `discovered -> approved -> configured -> active -> revoked`
   com auditoria e dashboard sem depender de F5.
4. Backup/restore local: configuracao, usuarios, mapa de instalacao, automacoes e rede.
5. Auditoria de produto: eventos importantes separados de log tecnico.
6. OTA Foundation: base controlada para validacao, agendamento, rollback e permissao.
7. API publica organizada: separar runtime, administracao, diagnostico e configuracao.
8. Documentacao minima: instalacao, primeiro acesso, usuarios, adocao, perfis,
   fail-safe, backup/restore e limitacoes conhecidas.

## Ordem Pratica Atual

Comecar por fail-safe por canal, porque ele transforma o ENDAP de sistema funcional
em produto mais seguro para campo. O escopo deve ficar fora do hot path critico e
usar as rotas ja existentes de comando de IO, automacao, comando remoto e dashboard.

## Regras Da Implementacao

- Nao alterar `control_loop`, scheduler critico, `cluster_manager`, `cluster_failover`
  ou `fieldbus` sem necessidade objetiva.
- Fail-safe vence comando manual.
- Em boot/restart, politica de startup vence snapshot/restauracao.
- A dashboard deve mostrar a origem efetiva da regra aplicada quando disponivel.
- A primeira entrega pode ser fundacao incremental, mas deve compilar e ser integravel.

## Progresso Registrado

### 2026-04-26 - Fundacao de Fail-safe por Canal

- Novo componente `components/runtime/failsafe` com politicas persistidas em NVS.
- Politica inicial por saida: startup, perda de comunicacao, valor seguro e rearme manual.
- Boot das saidas passa pela politica de startup antes de aplicar snapshot.
- Comando manual local, automacao local e comando remoto recebido respeitam bloqueio por
  fail-safe ativo com rearme manual.
- `/api/status` expoe origem efetiva e campos de fail-safe por saida.
- APIs adicionadas: `GET /api/failsafe`, `POST /api/failsafe/save`,
  `POST /api/failsafe/rearm`.
- Dashboard mostra estado de fail-safe como detalhe operacional do canal.
- Validado com `node --check` do script da dashboard e `idf.py build`.

Pendencia conhecida: disparo automatico de fail-safe por perda real de mestre/comunicacao
ainda precisa ser conectado aos eventos oficiais de failover/comunicacao em uma proxima
rodada, sem invadir o hot path critico.

### 2026-04-26 - Aperto de Seguranca V2

- `/api/status` deixou de ser leitura publica completa e agora exige `dashboard_read`.
- Adicionado `GET /api/public/status` com resposta reduzida para a dashboard ler nos
  remotos sem expor diagnostico completo.
- Dashboard passou a buscar status remoto por `/api/public/status`.
- Logout agora entra na trilha auditavel como `logout`.
- Validado com `node --check` do script da dashboard e `idf.py build`.

Pendencias conhecidas de Seguranca V2: armazenamento de auditoria ainda e em memoria,
nao ha backup/restore de contas/segredos e nao ha ainda politica avancada por origem
local/remota para acoes criticas.

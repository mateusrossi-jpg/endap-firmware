# ENDAP Studio — referência da dashboard embarcada

## Objetivo

Este documento registra a referência funcional da dashboard embarcada do firmware ENDAP para orientar a primeira versão do `endap-studio` como aplicação Web/PWA local. A documentação não altera kernel, runtime, fail-safe, scheduler, dashboard embarcada ou qualquer lógica crítica do firmware.

## Escopo da varredura

A referência foi organizada a partir da arquitetura atual do firmware ENDAP, incluindo dashboard embarcada, servidor HTTP local, APIs REST, cluster, registro de nós, fail-safe, I/O e diagnósticos. Quando uma rota ou estrutura ainda não estiver estabilizada no firmware, ela deve ser tratada no Studio como contrato conceitual ou mock até validação direta no gateway.

## Visão geral da dashboard embarcada atual

A dashboard embarcada existe como interface técnica local do gateway ESP32. Ela cumpre bem o papel de operação, teste, visualização rápida e manutenção direta no dispositivo, mas deve permanecer leve por causa das limitações de memória, armazenamento e processamento do ESP32.

A dashboard atual concentra funções como:

- monitoramento operacional do gateway;
- visualização de nós e cluster;
- gerenciamento de entradas e saídas;
- automação básica/regras;
- alertas e diagnóstico;
- políticas de fail-safe por saída;
- adoção/aprovação de nós pendentes;
- autenticação/sessão local.

## Telas, abas e blocos encontrados/esperados

### Monitoring

Função atual: visão rápida do estado operacional do sistema.

Dados reaproveitáveis para o Studio:

- status online/offline;
- uptime;
- estado geral do gateway;
- contadores resumidos de nós;
- alertas ativos;
- métricas de runtime quando disponíveis.

Migração recomendada:

- `Monitoring` → `Visão Geral` e `Diagnóstico`.

### Automation

Função atual: configuração/visualização de automações e regras simples.

Dados reaproveitáveis:

- lista de regras;
- modos de regra como `FOLLOW`, `PULSE_MS`, `ON_DELAY_MS`, `OFF_DELAY_MS`, `TOGGLE`, `FORCE_ON`, `FORCE_OFF`;
- relação IF/THEN entre entrada, condição e saída;
- status habilitada/desabilitada.

Migração recomendada:

- `Automation` → `Automação / Regras`.
- Nesta fase, o Studio deve ter editor simples mockado; Ladder avançado fica fora do V0.

### IO

Função atual: visualização e ajuste de pontos de entrada/saída.

Dados reaproveitáveis:

- alias;
- GPIO/endereço lógico;
- direção (`input`/`output`);
- estado atual;
- modo manual/teste;
- flags de reservado/protegido;
- associação a nó/gateway.

Migração recomendada:

- `IO` → `Entradas e Saídas`.
- GPIOs reservados devem aparecer bloqueados visualmente no Studio.

### Cluster

Função atual: visão técnica dos nós, transporte e saúde do cluster.

Dados reaproveitáveis:

- gateway local;
- nós remotos;
- perfil do nó;
- transporte ativo, como Wi-Fi, Ethernet/local ou futuro RS485;
- heartbeat;
- estado operacional/aprovação;
- nós pendentes.

Migração recomendada:

- `Cluster` → `Nós / Cluster`.

### Alerts

Função atual: exibir falhas, avisos operacionais e estados degradados.

Dados reaproveitáveis:

- severidade;
- mensagem;
- origem;
- data/hora;
- confirmação/ack quando existir;
- vínculo com nó, I/O ou subsistema.

Migração recomendada:

- `Alerts` → `Central de Alertas`.

### Diagnostics

Função atual: diagnóstico técnico do gateway e subsistemas.

Dados reaproveitáveis:

- heap/memória;
- jitter;
- ciclo/runtime;
- overrun;
- latência de comunicação;
- erros recentes;
- logs resumidos.

Migração recomendada:

- `Diagnostics` e `Runtime metrics` → `Observabilidade` e `Diagnóstico`.

### Fail-safe

Função atual: políticas conservadoras por saída para boot, perda de comunicação e falhas de runtime.

Dados reaproveitáveis:

- saída lógica;
- política habilitada/desabilitada;
- `boot_action`;
- `comm_loss_action`;
- `runtime_fault_action`;
- `safe_value`;
- `recovery_mode`;
- `failsafe_active`;
- `last_reason`;
- último valor aplicado.

Migração recomendada:

- `Fail-safe` → `Segurança Operacional`.

### Nodes/adoption

Função atual: fluxo de descoberta, aprovação, ativação e revogação de nós.

Dados reaproveitáveis:

- nós descobertos;
- nós pendentes;
- aprovação manual;
- perfil de nó;
- transporte;
- status operacional;
- último heartbeat.

Migração recomendada:

- `Node registry/adoption` → `Comissionamento`.

## Mapa mínimo de migração

| Dashboard embarcada | ENDAP Studio |
| --- | --- |
| Monitoring | Visão Geral / Diagnóstico |
| Automation | Automação / Regras |
| IO | Entradas e Saídas |
| Cluster | Nós / Cluster |
| Alerts | Central de Alertas |
| Runtime metrics | Observabilidade |
| Fail-safe | Segurança Operacional |
| Node registry/adoption | Comissionamento |

## Pontos fortes reaproveitáveis

- O firmware já possui separação conceitual entre gateway, nós, I/O, cluster e fail-safe.
- O fluxo de nós pendentes/aprovados é uma boa base para comissionamento profissional.
- As políticas de fail-safe por saída são um diferencial forte e devem ganhar tela própria no Studio.
- A dashboard embarcada serve como fonte de verdade para nomes, estados e endpoints.
- O Studio pode começar em mock sem exigir ESP32 conectado, desde que preserve os contratos futuros.

## Limitações da dashboard embarcada

- HTML/CSS/JS tende a ficar acoplado ao firmware.
- Memória e armazenamento do ESP32 limitam UX avançada.
- Não é ideal manter histórico persistente longo no gateway.
- Não há espaço adequado para editor visual completo, backup avançado e projeto local detalhado.
- A dashboard embarcada deve continuar leve, técnica e segura.
- Experiências ricas de desktop/mobile devem migrar para o Studio.

## Recomendações para Studio V0

- Criar shell Web/PWA escuro profissional.
- Usar mock data realista para gateway, nós, I/O, fail-safe, regras, diagnósticos e alertas.
- Criar camada `endapApi.ts` com nomes de funções iguais aos endpoints desejados.
- Preparar troca futura entre mock e API real.
- Não criar cloud, conta online, marketplace, SCADA completo ou Ladder avançado nesta fase.
- Manter a dashboard embarcada como fallback local e painel técnico leve.

## Recomendações para Studio futuro

- Conectar via REST/WebSocket ao gateway ENDAP.
- Adicionar histórico local de projeto/instalação.
- Criar backup/export/import de projeto `.endap.json`.
- Evoluir com editor visual de automação em fases.
- Preparar diagnóstico avançado e relatórios técnicos.
- Separar recursos profissionais do Studio sem aumentar o peso do firmware.

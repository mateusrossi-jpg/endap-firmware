# PLANS.md — ENDAP Execution Plans

## 📌 Objetivo

Este arquivo define o padrão de planejamento obrigatório antes de qualquer implementação relevante no ENDAP.

Ele existe para proteger:
- o kernel determinístico
- a clareza arquitetural
- o escopo ativo do produto
- a estabilidade da base durante a fase de fechamento do ENDAP v1

Utilizar para:
- refatorações
- mudanças no kernel
- melhorias estruturais
- onboarding / network / dashboard
- integração entre runtime e services
- alterações em transporte, registry, templates e persistência

---

## 🧭 Princípio central

**Nenhuma mudança importante deve ser implementada sem plano.**

Se a tarefa não puder ser explicada de forma simples, com escopo pequeno e validação objetiva, ela ainda não está pronta para implementação.

---

## 🧩 Plano 01 — Budget no Event Bus

### 🎯 Objetivo
Impedir que a fase de eventos drene toda a fila em um único ciclo e cresça sem limite temporal dentro do scheduler.

A intenção é preservar o determinismo do loop principal, tornando o processamento de eventos explicitamente limitado por ciclo.

---

### 🏷️ Tipo da tarefa
- runtime
- kernel
- hardening

---

### 📦 Escopo

#### Inclui:
- `components/runtime/event_bus/event_bus.c`
- `components/runtime/event_bus/include/event_bus.h`
- ponto de chamada da fase `Events` no scheduler

#### NÃO inclui:
- reescrita completa do event bus
- mudança de semântica dos eventos
- novo framework de filas
- alteração das automações

#### Fora de escopo:
- priorização de tipos de evento
- múltiplas filas por classe de evento
- event bus distribuído
- persistência de eventos

---

### 🧱 Camadas afetadas
- Runtime
- Kernel

---

### ⚠️ Riscos
- quebrar compatibilidade com o fluxo atual de eventos
- introduzir perda de eventos se a lógica for implementada incorretamente
- alterar comportamento observável de automações encadeadas
- criar starvation se o budget for pequeno demais

---

### 🛠️ Estratégia
1. substituir o dispatcher “drain total” por processamento limitado por quantidade
2. manter a fila e a API atual o máximo possível
3. expor uma função clara, por exemplo `event_bus_dispatch_budgeted(max_events)`
4. fazer o scheduler chamar explicitamente a versão com limite
5. manter compatibilidade interna com handlers existentes

A abordagem preferida é **limite por número de eventos por ciclo**, e não por tempo em microssegundos, para manter a solução simples e auditável.

---

### 🔄 Etapas
1. localizar a chamada atual de `event_bus_dispatch()`
2. introduzir nova função budgeted
3. processar no máximo `N` eventos por ciclo
4. manter evento remanescente na fila para ciclos seguintes
5. revisar se handlers podem publicar novos eventos sem quebrar a lógica
6. ajustar scheduler para usar a nova função

---

### 🧪 Validação
- compila corretamente
- sistema sobe
- eventos continuam sendo processados
- automações ainda reagem corretamente
- jitter não aumenta
- fase `Events` deixa de ter crescimento descontrolado
- fila não perde eventos em uso normal

Validação prática:
- gerar múltiplos eventos em sequência
- observar que eles são drenados ao longo de ciclos sucessivos
- verificar que o sistema continua responsivo sem picos anormais

---

### 📏 Impacto esperado
- impacto esperado no hot path: pequeno e positivo
- impacto esperado em memória: nenhum
- impacto esperado em arquitetura: local e controlado

---

### ✅ Critério de conclusão
- dispatcher deixa de drenar tudo sem limite
- scheduler passa a usar budget explícito
- comportamento permanece estável
- mudança pequena, clara e revisável
- determinismo preservado ou melhorado

---

## 🧩 Plano 02 — Remover logs do Control Loop

### 🎯 Objetivo
Eliminar chamadas de log dentro do `control_loop`, transferindo o registro textual para contexto não crítico.

A meta é seguir a regra oficial do projeto: o loop crítico deve apenas coletar/atualizar métricas, e nunca realizar logging pesado diretamente.

---

### 🏷️ Tipo da tarefa
- kernel
- hardening
- refatoração

---

### 📦 Escopo

#### Inclui:
- `components/kernel/control_loop/control_loop.c`
- módulo auxiliar de métricas ou monitor já existente, se necessário

#### NÃO inclui:
- mudança na lógica do control loop
- alteração da frequência do loop
- mudança no mecanismo de medição
- alteração global do sistema de logs

#### Fora de escopo:
- reformulação completa de observabilidade
- novo subsistema de telemetria
- envio de métricas via rede

---

### 🧱 Camadas afetadas
- Kernel
- Runtime

---

### ⚠️ Riscos
- perder visibilidade de métricas se o desacoplamento for mal feito
- introduzir duplicação de buffer/estado
- manter log residual em caminho crítico por descuido

---

### 🛠️ Estratégia
1. localizar todos os `ESP_LOG*` dentro do loop crítico
2. substituir logs diretos por atualização de snapshot/flag/estrutura simples
3. delegar o log real para task auxiliar ou módulo não crítico
4. manter o conteúdo das métricas atuais, mas fora do hot path

---

### 🔄 Etapas
1. mapear logs no `control_loop`
2. definir estrutura mínima de snapshot para exportação
3. mover emissão textual para contexto não crítico
4. revisar se a periodicidade de log continua suficiente
5. testar boot e execução contínua

---

### 🧪 Validação
- compila corretamente
- sistema sobe
- logs continuam aparecendo
- loop crítico não chama mais `ESP_LOG*`
- jitter não piora
- nenhuma regressão funcional

Validação adicional:
- revisar o código final para garantir ausência de logging no trecho crítico

---

### 📏 Impacto esperado
- impacto esperado no hot path: positivo
- impacto esperado em memória: pequeno
- impacto esperado em arquitetura: local

---

### ✅ Critério de conclusão
- não existe mais log textual no `control_loop`
- métricas continuam disponíveis
- observabilidade permanece útil
- mudança fica pequena e compreensível

---

## 🧩 Plano 03 — Sincronização explícita no Watchdog

### 🎯 Objetivo
Adicionar proteção de concorrência ao watchdog para evitar acesso simultâneo inconsistente em ambiente dual-core / multitask.

A meta é endurecer o módulo sem transformá-lo em uma abstração pesada.

---

### 🏷️ Tipo da tarefa
- kernel
- hardening

---

### 📦 Escopo

#### Inclui:
- `components/kernel/watchdog/watchdog.c`
- `components/kernel/watchdog/include/watchdog.h` se necessário

#### NÃO inclui:
- redesign do watchdog
- troca do mecanismo de supervisão
- integração com watchdog externo
- mudança do contrato funcional do módulo

#### Fora de escopo:
- distributed watchdog
- health manager avançado
- integração com cluster health

---

### 🧱 Camadas afetadas
- Kernel

---

### ⚠️ Riscos
- criar lock excessivo em trecho sensível
- introduzir deadlock por uso incorreto de primitivas
- alterar comportamento de feed/check/register

---

### 🛠️ Estratégia
1. manter o watchdog simples
2. proteger tabela e contadores compartilhados com mecanismo leve
3. usar seção crítica curta, preferencialmente com `portMUX_TYPE`
4. revisar `register`, `feed` e `check`

A proteção deve ser curta e localizada, sem inflar o módulo.

---

### 🔄 Etapas
1. mapear estruturas compartilhadas
2. adicionar primitive de sincronização no módulo
3. envolver operações críticas mínimas
4. revisar se há chamadas em ISR ou contexto especial
5. validar build e execução

---

### 🧪 Validação
- compila corretamente
- sistema sobe
- watchdog continua registrando e alimentando corretamente
- não há crash ou travamento
- não há impacto perceptível no loop
- comportamento do watchdog permanece igual do ponto de vista funcional

---

### 📏 Impacto esperado
- impacto esperado no hot path: nenhum ou mínimo
- impacto esperado em memória: nenhum
- impacto esperado em arquitetura: local

---

### ✅ Critério de conclusão
- acesso compartilhado do watchdog está protegido
- módulo continua simples
- sem regressão funcional
- mudança pequena e segura

---

## 🧩 Plano 04 — Formalizar contrato do State Engine

### 🎯 Objetivo
Reduzir o risco de acoplamento e concorrência indevida no `state`, formalizando quem pode escrever, em que contexto, e quais side effects são aceitáveis.

A meta imediata não é reescrever o módulo, mas **endurecer seu contrato arquitetural**.

---

### 🏷️ Tipo da tarefa
- runtime
- refatoração
- hardening

---

### 📦 Escopo

#### Inclui:
- `components/data/state/state.c`
- `components/data/state/include/state.h`
- revisão dos chamadores principais de `state_set_int()`
- documentação/comentários de contrato

#### NÃO inclui:
- reescrita total do state engine
- criação de nova arquitetura de storage
- separação completa entre storage e side effects
- mudança ampla no modelo de automação

#### Fora de escopo:
- event sourcing
- state replication distribuída
- locks globais para todo acesso ao state

---

### 🧱 Camadas afetadas
- Runtime
- Services

---

### ⚠️ Riscos
- quebrar caminhos atuais de atualização de estado
- espalhar wrappers demais
- gerar falsa sensação de segurança sem definir política real

---

### 🛠️ Estratégia
1. mapear quem escreve no `state`
2. distinguir claramente escrita vinda do loop, de fila interna e de services
3. documentar política de single-writer prioritário
4. restringir escrita direta em contextos indevidos
5. manter side effects atuais apenas se necessários para compatibilidade

A ideia é primeiro **disciplinar**, depois, se necessário, refatorar.

---

### 🔄 Etapas
1. localizar todos os chamadores de `state_set_int()`
2. classificar por contexto de execução
3. registrar a política oficial do módulo
4. reduzir chamadas indevidas vindas de services, se existirem
5. adicionar comentários/assertivas leves quando fizer sentido

---

### 🧪 Validação
- compila corretamente
- sistema sobe
- automação continua funcionando
- IO continua reagindo corretamente
- não há regressão em snapshot/event bus/output hook
- contrato do módulo fica explícito no código

Validação importante:
- revisar manualmente os chamadores de escrita
- confirmar que o loop principal permanece o escritor prioritário

---

### 📏 Impacto esperado
- impacto esperado no hot path: nenhum ou mínimo
- impacto esperado em memória: nenhum
- impacto esperado em arquitetura: controlado e positivo

---

### ✅ Critério de conclusão
- contrato do `state` está explícito
- chamadores perigosos foram identificados e reduzidos
- side effects continuam compreensíveis
- módulo fica menos propenso a acoplamento silencioso

---

## 🧩 Plano 05 — Limpeza de placeholders e inicializações duplicadas

### 🎯 Objetivo
Remover pequenos sinais de dívida técnica controlável, como módulos vazios em caminho principal e inicializações duplicadas.

A meta é limpar a base para continuidade das implementações no Codex sem confusão estrutural.

---

### 🏷️ Tipo da tarefa
- refatoração
- hardening

---

### 📦 Escopo

#### Inclui:
- `components/runtime/deterministic_snapshot/deterministic_snapshot.c`
- pontos de inicialização duplicados, como `runtime_monitor_init()`
- revisão leve de arquivos com responsabilidade ambígua

#### NÃO inclui:
- redesign arquitetural
- mudança de pipeline do sistema
- criação de novas features
- quebra de compatibilidade ampla

#### Fora de escopo:
- reestruturação grande do boot
- extração ampla de módulos
- reformulação do monitoramento

---

### 🧱 Camadas afetadas
- Runtime
- Kernel

---

### ⚠️ Riscos
- remover algo ainda usado de forma indireta
- introduzir falha de boot por limpeza apressada
- mexer em composição demais para uma tarefa pequena

---

### 🛠️ Estratégia
1. identificar placeholders reais
2. decidir entre:
   - remover do caminho principal
   - manter como stub documentado
3. eliminar inicializações duplicadas
4. preservar comportamento atual

---

### 🔄 Etapas
1. revisar `deterministic_snapshot`
2. decidir se ele deve ser removido do fluxo atual ou documentado como stub
3. localizar duplicações de init
4. manter apenas um ponto oficial de inicialização por módulo
5. validar boot e serviços

---

### 🧪 Validação
- compila corretamente
- sistema sobe
- nenhum módulo necessário deixa de iniciar
- não há init duplicado restante
- código fica mais claro para revisão e continuidade

---

### 📏 Impacto esperado
- impacto esperado no hot path: nenhum
- impacto esperado em memória: nenhum
- impacto esperado em arquitetura: local e positivo

---

### ✅ Critério de conclusão
- placeholders problemáticos foram resolvidos
- inicializações duplicadas foram eliminadas
- fluxo de boot permanece estável
- base fica mais limpa para próximas tarefas

---

## 🧩 Plano 06 — Onboarding v1 mínimo com node registry

### 🎯 Objetivo
Fechar a primeira fatia utilizável do fluxo de onboarding do ENDAP v1 no gateway, registrando nós descobertos, permitindo adoção, configuração básica e ativação sem tocar no hot path crítico.

A meta é transformar a descoberta de cluster já existente em um fluxo inicial de produto, pequeno e validável.

---

### 🏷️ Tipo da tarefa
- services
- onboarding
- gateway
- dashboard

---

### 📦 Escopo

#### Inclui:
- export do snapshot atual do cluster para consumo por services
- `node_registry` persistente com estados mínimos de adoção
- API HTTP mínima para listar nós, adotar, configurar e ativar
- UI mínima no dashboard para operar esse fluxo

#### NÃO inclui:
- reescrita do cluster manager
- protocolo distribuído novo entre gateway e field node
- template engine completa
- replicação avançada
- automação distribuída complexa

#### Fora de escopo:
- provisionamento remoto completo
- sincronização bidirecional rica de perfis
- wizard completo de onboarding
- múltiplos tipos de template com semântica avançada

---

### 🧱 Camadas afetadas
- Cluster / Services
- Runtime HTTP
- Dashboard

---

### ⚠️ Riscos
- acoplar onboarding demais ao cluster baseline
- persistir informação efêmera como se fosse configuração estável
- crescer demais o dashboard para uma primeira fatia
- confundir estado de conectividade com estado de adoção

---

### 🛠️ Estratégia
1. manter a descoberta atual como fonte de presença/saúde
2. criar um registro separado para o ciclo de produto do nó
3. persistir apenas o que é necessário para adoção/configuração mínima
4. expor APIs simples e auditáveis
5. adicionar uma UI pequena no dashboard reaproveitando a linguagem já existente

A separação importante é:
- **cluster** continua dizendo quem está vivo / suspeito / offline
- **node registry** passa a dizer se o nó foi descoberto / adotado / configurado / ativado

---

### 🔄 Etapas
1. exportar snapshot compacto do `cluster_manager`
2. implementar `node_registry` com persistência mínima em NVS
3. sincronizar presença do cluster no registry fora do hot path
4. expor `/api/nodes`
5. expor ações mínimas de onboarding: adotar, configurar, ativar
6. renderizar lista simples de nós no dashboard
7. validar build e integração local

---

### 🧪 Validação
- compila corretamente
- sistema sobe
- kernel e control loop permanecem intocados no comportamento crítico
- nó descoberto aparece no registry
- operador consegue adotar
- operador consegue salvar perfil/template mínimo
- operador consegue ativar
- configuração persiste após reboot

Validação prática:
- subir dois ESP32 na mesma rede
- confirmar descoberta do peer
- adotar e ativar pelo dashboard
- reiniciar o gateway e confirmar persistência do registry

---

### 📏 Impacto esperado
- impacto esperado no hot path: nenhum
- impacto esperado em memória: pequeno e controlado
- impacto esperado em arquitetura: positivo e alinhado ao escopo ativo

---

### ✅ Critério de conclusão
- existe lista mínima de nós no gateway
- existe distinção entre presença de rede e estado de adoção
- operador consegue executar o fluxo básico `discover -> adopt -> configure -> activate`
- persistência mínima funciona
- mudança permanece local, compreensível e revisável

---

## 🧩 Plano 07 — Simplificar compreensão da Dashboard v1

### 🎯 Objetivo
Reduzir a carga cognitiva da dashboard atual sem perder a identidade visual nem remover capacidades importantes.

A meta é tornar a leitura inicial mais óbvia para operador e banca: primeiro entender o estado do sistema, depois decidir a ação, e só então abrir detalhes técnicos.

---

### 🏷️ Tipo da tarefa
- dashboard
- ux
- produto

---

### 📦 Escopo

#### Inclui:
- simplificação da hierarquia visual do topo da dashboard
- unificação da linguagem principal para leitura mais clara
- resumo guiado com “o que olhar agora”
- ocultação de detalhes avançados por padrão
- reorganização da aba de cluster para priorizar onboarding

#### NÃO inclui:
- reescrever dashboard do zero
- trocar framework/UI stack
- criar navegação multi-página
- mudar APIs além do necessário para leitura da UI atual

#### Fora de escopo:
- Studio completo
- sistema de permissões
- redesign completo de identidade visual
- analytics avançado de uso

---

### 🧱 Camadas afetadas
- Dashboard
- Runtime HTTP apenas se a leitura da UI exigir novo dado simples

---

### ⚠️ Riscos
- simplificar demais e esconder informação útil
- misturar melhoria de compreensão com refatoração grande
- quebrar strings/HTML do dashboard embarcado por edição extensa

---

### 🛠️ Estratégia
1. preservar o visual base já aprovado
2. reorganizar a informação em níveis:
   - estado geral
   - próxima ação
   - detalhes técnicos
3. deixar o modo avançado explícito e opcional
4. priorizar português claro nas áreas principais
5. manter mudanças localizadas em `dashboard.c`

---

### 🔄 Etapas
1. simplificar hero, abas e títulos principais
2. adicionar card de leitura guiada
3. esconder tendência/validação e ownership técnico por padrão
4. priorizar onboarding na aba de cluster
5. validar build e leitura geral da UI

---

### 🧪 Validação
- compila corretamente
- dashboard continua carregando
- leitura do estado geral fica mais óbvia
- próxima ação do operador fica visível
- detalhes técnicos continuam acessíveis, mas não dominam a tela
- onboarding continua encontrável sem confusão

---

### 📏 Impacto esperado
- impacto esperado no hot path: nenhum
- impacto esperado em memória: nenhum ou mínimo
- impacto esperado em arquitetura: local e positivo

---

### ✅ Critério de conclusão
- dashboard fica mais compreensível sem perder funções importantes
- linguagem principal fica consistente
- monitoramento avançado deixa de competir com a leitura básica
- aba de cluster fica orientada a produto, não só a detalhe técnico

---

## 🔁 Fluxo de uso com Codex

Antes de implementar qualquer uma das tarefas acima:

1. selecionar apenas um plano
2. revisar escopo e fora de escopo
3. implementar a mudança mínima necessária
4. validar
5. registrar o resultado
6. só então partir para o próximo plano

---

## 🚫 Anti-padrões

- implementar dois ou mais planos ao mesmo tempo
- aproveitar a tarefa para “melhorar outras coisas”
- tocar no kernel para resolver problema de service/UI
- misturar refatoração estrutural com feature nova
- aumentar abstração sem necessidade
- crescer o escopo após o início

---

## 🔥 Regra de ouro

**"Se a tarefa não cabe em um plano simples, ela está grande demais."**

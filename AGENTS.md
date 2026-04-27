# AGENTS.md — ENDAP Platform

## 📌 Projeto

ENDAP — Edge-Native Distributed Automation Platform

Plataforma embarcada distribuída para automação com execução determinística local, operação local-first, onboarding guiado de nós e evolução incremental para uma arquitetura profissional escalável.

---

## 🎯 Objetivo atual

Estamos na fase de **fechamento do ENDAP v1 como produto utilizável**, priorizando:

1. Estabilidade do núcleo já validado
2. Simplicidade de adoção
3. Clareza arquitetural
4. Rede funcional entre gateway e nós
5. Dashboard mínima de adoção e operação
6. Preparação para demonstração (TCC + produto inicial)

O foco atual **não é expandir o kernel**, mas transformar a base já construída em uma versão mínima coerente de produto.

---

## 🧱 Arquitetura oficial atual

Camadas principais:

- **Kernel** — determinístico, crítico, local
- **Runtime** — execução local, automação, estado, integração com IO
- **Services** — rede, onboarding, dashboard, registry, transporte
- **UI** — dashboard embarcada e base futura para Studio

Separação obrigatória:
- o **Kernel** não deve depender diretamente de Services
- Services podem observar, comandar e integrar com o Runtime
- recursos de produto devem ser adicionados **fora do hot path crítico**

---

## 🧭 Escopo ativo oficial

O escopo ativo de implementação é:

1. **ENDAP Onboarding v1**
2. **ENDAP Network v1**
3. **Gateway v1**
4. **Field Node v1**
5. **Dashboard v1 de adoção e operação**
6. **Perfis de nó v1**
7. **Templates v1**
8. **Clonagem/replicação básica**
9. **Persistência mínima**
10. **Recovery/reconexão básica**
11. **Kernel Product Freeze v1**

---

## 🔒 Kernel (CRÍTICO)

Componentes críticos já consolidados:

- control_loop (1 ms)
- scheduler por fases
- state engine
- event bus
- integração com IO
- watchdog / supervisão
- observabilidade de determinismo

Pipeline principal:
`IO → Fieldbus → Automation → Events → Diagnostics`

### Regras obrigatórias do kernel

- NÃO alterar lógica do kernel sem necessidade explícita
- NÃO adicionar features fora do escopo da tarefa
- NÃO usar malloc/free no hot path
- NÃO inserir logs pesados no loop crítico
- NÃO mover código crítico para caminhos lentos
- NÃO acoplar services diretamente ao kernel
- NÃO misturar refatoração ampla com feature nova
- NÃO aumentar complexidade do loop de 1 ms sem justificativa clara

### Objetivo do kernel nesta fase

O kernel deve ser tratado como **base congelando para produto**, com mudanças apenas quando forem:

- pequenas
- justificadas
- auditáveis
- validadas
- sem regressão temporal

---

## 🧠 Runtime

Responsabilidades típicas do Runtime:

- execução local da lógica
- automação
- estado interno
- integração com IO local
- integração com fieldbus
- aplicação de comandos vindos da UI / rede por caminhos seguros
- suporte à persistência mínima e restauração de estado

O Runtime pode evoluir, desde que preserve:
- previsibilidade
- simplicidade
- baixo acoplamento
- compatibilidade com o núcleo determinístico

---

## 🌐 Services (fora do kernel)

Services atualmente relevantes ao produto:

- Wi-Fi
- HTTP / WebSocket
- dashboard
- onboarding
- node registry
- descoberta / adoção de nós
- cluster básico
- abstrações de transporte
- persistência de configuração
- APIs de operação

### Diretriz de arquitetura

Services devem:
- operar fora do hot path
- interagir por interfaces claras
- não impor dependência indevida ao kernel
- preservar a separação entre controle determinístico e serviços de produto

---

## 🧩 Perfis de nó e visão de produto

A plataforma deve operar como um sistema coerente desde um único nó até múltiplos nós distribuídos.

Perfis iniciais relevantes nesta fase:

- Gateway
- Field Node
- Relay Node
- Sensor Node
- Local I/O Node

Diretrizes:
- usar **base única de código**
- habilitar comportamento por **perfil**
- evitar forks desnecessários
- manter onboarding simples e orientado por perfil/template

---

## 🔌 Comunicação e transportes

A plataforma foi concebida para suportar múltiplos transportes, mas a ativação deve ser seletiva por nó.

Transportes relevantes para a fase atual:
- Wi-Fi
- RS485
- Ethernet/RJ45 (preparação e evolução)
- contrato comum de transporte

Fora do escopo imediato de implementação:
- LoRa
- Wi-Fi Mesh obrigatório
- multi-transporte pleno em produção
- fallback avançado entre todos os enlaces

### Regra de ouro

Cada nó deve inicializar apenas os drivers, tasks, filas e serviços dos transportes realmente habilitados pelo seu papel/configuração.

---

## 🛠️ Onboarding v1

O produto atual deve evoluir para uma experiência onde:

1. o nó sobe com identidade inicial
2. o gateway o detecta
3. o operador adota o nó
4. escolhe um perfil
5. aplica um template
6. testa IO
7. ativa o nó

Blocos relacionados:
- bootstrap universal
- node registry
- estados de adoção
- templates básicos
- persistência mínima
- recovery / reconexão

---

## 📊 Observabilidade

Módulos existentes / esperados nesta fase:

- determinism_probe
- kernel_metrics
- kernel_trace
- phase_monitor
- bus_health_monitor
- métricas de node/network quando aplicável

Métricas prioritárias:

- jitter (min/avg/max)
- tempo de execução
- overrun
- deadline miss
- latência
- timeout / retry
- estado do nó
- estado de link/transporte
- health básico operacional

---

## 🔧 Hardware alvo

Atual:
- ESP32-WROOM-32

Preparado para evolução futura:
- ESP32-S3 (PSRAM)
- outros MCUs via HAL

### Diretriz prática

Nesta fase, o desenvolvimento deve continuar orientado ao hardware **real disponível**, evitando abstrações prematuras para múltiplos MCUs.

---

## 🧩 Regras de desenvolvimento

- código simples e auditável
- evitar overengineering
- manter separação de camadas
- funções pequenas e claras
- nomes consistentes
- mudanças pequenas e localizadas
- priorizar legibilidade operacional
- preferir compatibilidade incremental em vez de reescrita ampla
- sempre explicitar impacto em determinismo, acoplamento e validação

---

## 🧪 Definition of Done

Uma tarefa só está concluída se:

- compila sem erro
- sistema sobe corretamente
- não quebra o control_loop
- não altera determinismo sem justificativa e medição
- mudança é pequena e compreensível
- possui instrução de validação
- não introduz acoplamento indevido
- mantém coerência com o escopo ativo do ENDAP v1

---

## 🔍 Review guidelines

Ao revisar código:

- identificar riscos de tempo real
- verificar impacto no hot path
- procurar acoplamento desnecessário
- sugerir simplificações
- evitar abstrações desnecessárias
- checar se a mudança realmente pertence ao escopo atual
- checar se a tarefa está pequena o suficiente
- recusar expansão implícita de escopo

---

## ⚙️ Comandos do projeto

(Substituir conforme seu ambiente)

- build: `idf.py build`
- flash: `idf.py flash`
- monitor: `idf.py monitor`

---

## 🚀 Estratégia atual

Sequência prática atual:

1. preservar o kernel estável
2. fechar a base mínima de produto
3. concluir onboarding e operação básica
4. validar rede funcional gateway ↔ nós
5. validar hardware real
6. refinar UX e documentação de operação
7. congelar versão demonstrável

---

## ❌ NÃO FAZER AGORA

- não implementar LoRa
- não implementar Wi-Fi Mesh como requisito obrigatório
- não criar Studio completo
- não expandir features do kernel sem necessidade clara
- não portar para múltiplos MCUs agora
- não perseguir cloud como prioridade
- não introduzir automação distribuída complexa nesta fase
- não misturar arquitetura futura com escopo ativo de produto v1

---

## 🧾 Filosofia

**"Simples, determinístico, robusto, utilizável e evolutivo."**

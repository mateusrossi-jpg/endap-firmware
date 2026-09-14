# Relatório de Auditoria: Estado Atual do ENDAP (TCC)

## 1. Estrutura Real do Firmware
O firmware possui uma arquitetura baseada em componentes locais do ESP-IDF, divididos em camadas lógicas principais de acordo com as especificações atuais (`AGENTS.md`):
- `main/`: Ponto de entrada do sistema.
- `components/`: Contém os subsistemas modulares.
  - `kernel/`: Laço crítico e agendador determinístico.
  - `runtime/`: Máquina de regras, event bus, failsafe, drivers I/O e comunicação de rede.
  - `data/`: Estruturas de imagem de I/O e estado.
  - `hal/` e `drivers/`: Abstração de hardware.
  - `dashboard/`: UI embarcada.
  - Outros: `observability`, `onboarding`, `security`, `comm`.

## 2. Componentes Existentes
O repositório possui forte separação conceitual. Os principais blocos são identificados nas pastas `kernel`, `runtime`, `comm`, `dashboard` e `hal`. A pasta `v3` existe e agrupa implementações de arquitetura futura.

## 3. Componentes Efetivamente Compilados
De acordo com o `CMakeLists.txt` raiz, os seguintes componentes estão sendo inseridos ativamente no build:
- **Kernel**: `core`, `control_loop`, `control_kernel`, `scheduler`, `watchdog`
- **Runtime**: `deterministic_snapshot`, `fieldbus`, `rs485_engine`, `rs485_master`, `automation` (Rule Engine), `failsafe`, `event_bus`, `io_driver`, `http_server`, `network_v1`, etc.
- **Data**: `io_image`, `state`
- **Comm**: `parser`, `protocol`
- **Dashboard**: `dashboard`
- **Observability**: diversas probes de determinismo

## 4. Componentes Fora do Build
- Todo o escopo de `components/v3` foi ativamente excluído através da diretiva `set(EXCLUDE_COMPONENTS v3)` no `CMakeLists.txt` raiz. 

## 5. Estado Real do Controller
O loop de controle do Controller (`kernel/control_loop`) está intacto e desacoplado dos serviços pesados de rede. A task principal de inicialização (`endap_boot_task`) inicia os serviços em núcleos adequados, mantendo as premissas do TCC (execução determinística).

## 6. Estado Real do Field Node
Suportado pela stack `fieldbus` e `rs485_engine/master`. O Field Node atua primariamente como expansão de I/O através do barramento RS-485, respeitando o conceito da Fase 3: expansão autônoma, sem lógicas de roteamento ou controle pesadas.

## 7. Estado Real do Rule Engine
O `automation_engine.c` está presente e atrelado ao `runtime/automation`. Provê o comportamento lógico/funcional local sobre as entradas sem depender de conexões externas.

## 8. Estado Real do Failsafe
O `failsafe.c` está ativo e embutido no processo de I/O. Garante estados seguros de saída em caso de perda de comunicação ou falhas de ciclo de loop de controle.

## 9. Estado Real do Event Bus
Construído sobre `event_bus.c`, está ativo para desacoplar as alterações de estado das regras de automação das camadas assíncronas (rede/UI).

## 10. Estado Real dos Drivers de I/O
Implementados em `io_driver.c` com amarração em `hal/`. Atualizações trafegam via `io_image` preservando as restrições do hot path.

## 11. Estado Real do RS-485
Composto por `rs485_engine.c` (gestão do enlace), `rs485_master.c` (controle mestre) e a camada HAL (em `hal/rs485`).

## 12. Estado Real do Protocolo ENDAP
Implementado nas camadas `comm/protocol` e `comm/parser` para padronizar as mensagens que trafegam pelo meio serial ou Wi-Fi.

## 13. Estado Real do Onboarding/Claim
Componente `onboarding` isolado e incluído no build para suportar adoção de Field Nodes pelo Controller ou adoção do Controller pelo ambiente de rede.

## 14. Estado Real da Dashboard
Está congelada no modo Manutenção. Compilada diretamente como um binário comprimido (`index.html.gz.S`) exposto e servido pelo `http_server`.

## 15. APIs Existentes
APIs expostas sobre HTTP pelo módulo `http_server`. As APIs gerenciam o fluxo de adoção, injeção de configurações e leitura de status (diagnósticos).

## 16. Dependências Entre Componentes
A dependência arquitetural segue o fluxo correto da V1: o `kernel` depende apenas das estruturas de hardware e do controle de tempo; o `runtime` integra dados com `kernel`; e os `services` e `dashboard` são processos que apenas consomem ou produzem dados (assíncronos) via buffers e Event Bus.

## 17. Pontos Onde Dashboard/Rede Podem Afetar Controle Local
Através da modificação das estruturas em `components/data/` (ex: configurações de failsafe ou state/io_image) por intermédio das APIs expostas. A separação garante que requisições lentas não quebrem o ciclo estrito de 1ms de controle local.

## 18. Estado do CMake
Configurado estritamente para a V1, declarando e separando os componentes. Exclusão explícita de `v3`.

## 19. Estado do Build
O build executa sem bloqueios fatais (`build.log` recente aponta compilação bem sucedida: "Project build complete."). Tamanho final `ENDAP.bin` cabendo com folga na partição (aprox 23% livre no slot App).

## 20. Arquivos Críticos
- `main/main.c`
- `CMakeLists.txt`
- `components/kernel/control_loop/control_loop.c` (Assumido)
- `components/runtime/automation/automation_engine.c`
- `components/runtime/failsafe/failsafe.c`

## Classificação de Ação

| Componente | Classificação | Justificativa |
|---|---|---|
| **Kernel / Loop de Controle** | 🔵 CONGELAR | Estável, crítico, núcleo validador do TCC. |
| **Rule Engine (Automation)** | 🟢 PRESERVAR | Lógica local já atende o requisito de autonomia. |
| **Event Bus / Failsafe** | 🟢 PRESERVAR | Fundamentais para a demonstração da tese arquitetural. |
| **IO Driver / HAL** | 🟢 PRESERVAR | Sem necessidade de abstração extra nesta fase. |
| **Dashboard** | 🔵 CONGELAR | Apenas manutenções pontuais. Design não deve ser alterado radicalmente. |
| **Protocolos Avançados (Mesh, LoRa, ESP-NOW)** | 🔴 REMOVER DO CAMINHO CRÍTICO | Excluídos explicitamente; não ativam/interferem no sistema V1 (Já ausentes no build V1 via diretiva v3 exclusion). |
| **Gateway V2, Studio Cloud** | ⚪ FUTURO | Ficam integralmente postergados pós-TCC. |

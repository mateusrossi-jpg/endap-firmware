# Gap Analysis: Preparação para Demonstração do TCC ENDAP

## Tese que a demonstração deve provar
1. O Controller possui controle local.
2. O sistema possui I/O físico.
3. Existe lógica de automação local.
4. Existe Failsafe.
5. O Event Bus desacopla eventos da lógica.
6. A comunicação não é necessária para cada ciclo de controle.
7. O Controller pode se comunicar com Field Node.
8. O Field Node possui I/O e comportamento local.
9. A Dashboard permite supervisão.
10. A arquitetura permite expansão futura.

## Tabela de Gaps

| ITEM | ESTADO ATUAL | NECESSÁRIO PARA TCC | AÇÃO | PRIORIDADE |
|---|---|---|---|---|
| **Controller (Core / Loop)** | Compila com sucesso e inicializa tarefas de forma isolada. Loop em 1ms funcional. | Comprovar que atende inputs e muda outputs independentemente do estado da rede Wi-Fi/Dashboard. | Validar hardware real na bancada de demonstração. | P0 |
| **Dashboard UI** | Embutida via `.gz`, funcional porém carece de confirmação final de estabilidade das views. | Demonstrar abas básicas: Estado de Conexão, Monitor de Entradas/Saídas (I/O), e Diagnóstico Básico. | Limpar bugs ou bloqueios na UI apenas. NADA de novas features visuais ou designs. | P1 |
| **Integração Field Node (RS-485)** | Drivers `rs485_engine`, `rs485_master`, `hal/rs485` existem e estão compilados. | Mostrar um Field Node sendo descoberto ou comunicar IO pela porta serial (mesmo de forma simples). | Testar integração básica entre placa Controller e 1 Field Node via barramento físico RS485. | P1 |
| **Rule Engine (Automação Local)** | Ativo. Estruturas `automation_engine` presentes. | Criar/mostrar pelo menos 1 regra hardcoded ou injetada pela Dashboard. (Ex: Input 1 -> Aciona Output 2). | Configurar regra simples demonstrativa. | P0 |
| **Failsafe** | Presente e ativo. | Demonstrar que ao desconectar o Field Node ou interromper o laço, saídas entram no estado seguro. | Configurar timeout e estado de repouso nas portas essenciais para a demo. | P1 |
| **Protocolo de Rede / Onboarding** | Componentes de AP, Captive Portal e HTTP integrados. | Usuário se conectar no Wi-Fi ENDAP e acessar o IP de gerência sem precisar de rede externa. | Validar fluxo End-to-End da conexão local. | P0 |
| **Observabilidade de Determinismo** | Probes estão ativos. | Mostrar em logs ou aba de diagnóstico que o loop roda a 1ms consistentemente. | Capturar log serial ou na Dashboard durante a demonstração para prova empírica. | P2 |
| **LoRa / Mesh / Cloud** | Excluído do build `v3`. | NÃO DEVE SER ABORDADO no software de TCC, apenas citado na apresentação teórica como expansão. | Ignorar e manter excluído do build/deploy. | P3 |

## Conclusões
O sistema está excepcionalmente bem-posicionado em relação à arquitetura proposta. Não há gaps arquiteturais crônicos no Caminho Crítico. O esforço final será **validação de ponta a ponta na bancada**, **remoção de detritos da UI** (se existirem) e **configuração do ambiente de simulação/demonstração física**. O build já passa, indicando coerência sintática da V1.

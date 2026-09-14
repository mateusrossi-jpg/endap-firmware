# Auditoria de Timeout e Failsafe RS-485 (Phase 2B)

## 1. Comportamento Atual e Timeouts

Abaixo as predefinições de comunicação e interrupção de ciclo localizadas:
- **Intervalo de Idle Poll RS-485 (`IDLE_POLL_INTERVAL_US`)**: 4 ms.
- **Timeout de Resposta RS-485 (`RESPONSE_TIMEOUT_US`)**: 6 ms.
- **Contagem Máxima de Retry (`RS485_MAX_RETRY`)**: 3 tentativas sequenciais.
- **Timeout Global de Vida do Engine (`RS485_ENGINE_TIMEOUT_MS`)**: 100 ms.

### Qual é o comportamento quando:

**1. RS-485 funciona normalmente**
O nó do Controller (`rs485_master`) interroga via poll a comunicação. O Field Node responde com ACK num período bem menor que a janela de 6 ms. O Controller marca o respectivo nó como `ONLINE`, limpando tentativas de retry e estendendo a saúde do bus.

**2. Controller deixa de receber Field Node**
Caso o ACK atrase mais que 6 ms, o Controller incrementará uma variável de retentativa (retry). Se 3 verificações seguidas terminarem em falta de resposta sem ACK, o nó será declarado oficialmente como `OFFLINE` e o fluxo passará a aguardar a retomada de heartbeat.

**3. Field Node deixa de receber Controller**
Se o Field Node não receber pacotes adequados de seu controlador, o motor de rede interno (`rs485_engine_tick_1ms`) registrará a ausência durante sua própria contagem de watchdog. Ultrapassando o threshold de estabilidade global (100 ms), o nó se colocará forçadamente como `OFFLINE`.

**4. Dados chegam atrasados**
As janelas ultra-determinísticas farão com que as requisições lentas (maiores que 6 ms) sejam ignoradas e gerem timeout para a janela daquele poll. Se o atraso for recorrente, contudo as repostas acabarem chegando antes dos 3 retries, o sistema mantém o dispositivo online mas aumenta estatísticas de "timeout events" e "retry events".

**5. Dados deixam de chegar**
Ao confirmar o estopim de comunicação via `OFFLINE` event no controlador ou nó, o Fail-Safe interno notifica as entradas e saídas configuradas pela política do usuário, disparando internamente o motivo de `FAILSAFE_REASON_COMM_LOSS`. A depender do profile da saída, ela pode manter último estado (`Hold Last`), forçar queda segura (`Force OFF`), dentre outros.

**6. Comunicação retorna**
O Controller continua enviando sondagens no round-robin para todos os nós da tabela. O ACK restabelece de imediato a confiança ao longo de todo o sistema. Embora a rede volte à posição `ONLINE`, o Failsafe das saídas locais pode segurar a operação por medidas restritivas (requerendo rearme se `recovery_mode` for manual).

## 2. Parecer da Auditoria

**VALORES ATUAIS IDENTIFICADOS:**
- Timeout de Resposta Master: 6 ms
- Máximo de Retries Consecutivas: 3
- Intervalo entre requisições Idle: 4 ms
- Timeout do Motor: 100 ms

**RECOMENDAÇÃO IMEDIATA:**
- **Manter os valores originais intactos.**

**MOTIVO DA PRESERVAÇÃO:**
- As latências foram ajustadas com critério ultra-rígido de determinismo. Afrouxar a velocidade apenas disfarça problemas elétricos em TCCs. A proteção de hardware exige 100 ms para desarmar contatores defeituosos na vida real sem hesitação.

**IMPACTO PARA A DEMONSTRAÇÃO:**
- Será muito fácil simular e auditar para os avaliadores. Desconectar ou torcer os cabos das extremidades ativará visualmente todas as proteções mecânicas em um décimo de segundo, oferecendo sensação sólida de segurança à rede real.

# Auditoria de Fluxo de Automação (Phase 2A)

## 1. Como uma entrada física chega atualmente ao Event Bus
- O módulo `io_driver` realiza varreduras determinísticas periódicas (`io_driver_scan_inputs`).
- Para entradas digitais, o sinal do GPIO é lido, invertido se `active_low`, e submetido a um filtro de debounce (`debounce_samples`).
- Quando um sinal se mantém estável pelo número configurado de amostras e difere do estado anterior, o `io_driver` chama `state_set_int()`.
- O módulo `state` atualiza a representação na memória (`int_values`) e publica um evento no Event Bus com o tipo `EVENT_STATE_CHANGE`.

## 2. Como o Rule Engine recebe esse evento
- O módulo de automação (`automation_engine`) se inscreve no Event Bus durante a inicialização chamando `event_bus_subscribe(EVENT_STATE_CHANGE, ...)` passando um handler apropriado.
- Sempre que uma entrada muda de estado, o Event Bus invoca de forma orçamentada (budgeted dispatch) os handlers associados a esse tipo de evento.

## 3. Como uma regra existente produz uma ação
- Dentro do handler do `automation_engine`, as regras ativas na memória (armazenadas em estruturas `automation_node_t`) são avaliadas contra o novo estado recebido da entrada.
- A função de avaliação (`automation_eval_rule`) compara o valor de entrada com um limiar (`threshold`) usando um operador lógico (`gt`, `lt`, `eq`, etc.).
- Se a condição for verdadeira, a ação definida (`on_true` ou `on_false`) é disparada acionando `automation_apply_output()`.

## 4. Como a saída física é comandada
- O `automation_engine` chama a função `automation_dispatch_output()`.
- Uma verificação rigorosa de **Fail-Safe** é realizada. Se o comando não for interceptado pelo sistema de proteção, o estado lógico do output é alterado via `state_set_int(output, effective_value)`.
- O `state_set_int()` envia o evento e chama o hook atrelado em `io_driver`, chamado `state_output_changed()`, que por sua vez aciona uma flag na máscara `pending_mask`.
- O loop de controle restrito de tempo (determinístico) do `io_driver` processa um pino de saída por ciclo de tick a partir do `pending_mask`, modificando fisicamente o pino com `gpio_set_level()`.

## 5. Como o Failsafe interfere nesse fluxo
- Toda ação de automação local é checada pela função `failsafe_guard_command(output_id, value, FAILSAFE_COMMAND_AUTOMATION, ...)`.
- Se a política de Fail-Safe estiver ativa e num estado restritivo (ex. requerendo Rearme Manual após falha de comunicação), a tentativa de comando retorna como bloqueada (`AUTOMATION_ACTION_BLOCKED_FAILSAFE`) e ignora a mudança física para proteger o circuito.

## 6. Se já existe algum mecanismo de configuração de regras
- O firmware possui infraestrutura para armazenar blobs em memória flash através de NVS.
- Os blobs carregados na inicialização restauram estruturas completas de automação usando funções oficiais (`automation_persist()` e `automation_load()`).

## 7. Se já existe algum exemplo de regra utilizável
- Sim, o código de automação já contempla operações diretas e seguras sem hardcode local.
- É possível inserir uma regra de modo contínuo (Acompanhamento da entrada) sem tocar no núcleo:
  - `input`: ID da entrada configurada
  - `op`: `AUTOMATION_OP_EQ`
  - `threshold`: 1 (ligado)
  - `mode`: `AUTOMATION_MODE_FOLLOW`
  - `output`: ID da saída atrelada
  - `on_true`: 1
  - `on_false`: 0
- Esta regra atende à demonstração e pode ser inserida facilmente dentro do array no firmware ou persistida via API.

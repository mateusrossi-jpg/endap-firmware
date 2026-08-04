# Plano de Testes ENDAP v1 (Ponta a Ponta)

Este documento descreve o fluxo de validação da versão atual do produto ENDAP (ENDAP v1). Ele garante que as funcionalidades críticas de onboarding, clusterização, dashboard e persistência estejam operacionais sem regressões no kernel de tempo real.

## 🎯 Escopo do Teste

O foco é a adoção, resiliência de rede e persistência, abrangendo:
1. Boot do Gateway (ACTIVE)
2. Boot do Field Node (PENDING)
3. Descoberta e Claim Remoto
4. Persistência de estado entre reboots
5. Failover de transportes
6. Factory Reset e Retorno ao PENDING
7. Estabilidade do loop crítico (1 ms)

---

## 🛠️ Requisitos de Hardware e Software

- **Hardware:** Pelo menos 2 placas ESP32 ligadas na mesma rede Wi-Fi (ou via cabo RS485 se aplicável).
- **Rede:** Acesso à rede Wi-Fi configurada (ou AP Local do ESP32).
- **Monitoramento:** Ferramenta serial (ex: `idf.py monitor` ou `miniterm`) para acompanhar o log de cada nó.
- **Cliente HTTP:** Navegador web para o Dashboard e `curl` / Postman / `tools/test_api.sh` para acionar APIs de teste.

---

## 📋 Checklist de Validação Passo a Passo

### Fase 1: Setup Inicial & Boot

- [ ] **Passo 1.1:** Compilar o firmware com `idf.py build` (verificando `exit code 0`).
- [ ] **Passo 1.2:** Flashear o **Nó 1 (Gateway)** e abrir seu monitor serial (`idf.py -p PORT1 monitor`).
- [ ] **Passo 1.3:** Pelo Dashboard ou API local, realizar o onboarding do Nó 1 como `NODE_PROFILE_GATEWAY` e definir estado como `ACTIVE`.
- [ ] **Passo 1.4:** Flashear o **Nó 2 (Field Node)** e garantir que ele foi formatado (`idf.py erase-flash` ou acionar Factory Reset).
- [ ] **Passo 1.5:** Conectar ambos na mesma rede Wi-Fi.

**Pontos de Observação (Fase 1):**
- O Gateway deve exibir log: `Gateway ativo. Serviços de rede iniciados.`
- O Field Node deve exibir log: `Nó em estado PENDING, aguardando adoção.`
- O Dashboard do Gateway deve estar acessível via IP.
- Nenhuma mensagem de overrun do `control_loop` deve aparecer no boot inicial.

### Fase 2: Descoberta e Claim Remoto

- [ ] **Passo 2.1:** Acessar o Dashboard do **Gateway**.
- [ ] **Passo 2.2:** Verificar se o **Nó 2 (Field Node)** aparece na Lista de Nós (via `GET /api/nodes`).
- [ ] **Passo 2.3:** No Dashboard do Gateway, clicar em **"Adotar Nó"** no card do Nó 2.
- [ ] **Passo 2.4:** Preencher o Modal de Claim (Perfil: `Field Node`, Nome: `Sensor Sala`). Confirmar a adoção.
- [ ] **Passo 2.5:** Verificar a resposta visual de sucesso no Dashboard.

**Pontos de Observação (Fase 2):**
- **Log Gateway:** Deve exibir envio de mensagem broadcast de claim (`CLM1`) e registro no `node_registry`.
- **Log Field Node:** Deve exibir recebimento de `CLM1`, execução do `endap_onboarding_claim()` e mudança para estado `ACTIVE`.
- O Dashboard do Gateway deve atualizar automaticamente a lista e mostrar o nó como `ACTIVE`.

### Fase 3: Persistência e Reboot

- [ ] **Passo 3.1:** Resetar fisicamente (ou via pino EN/RST) o **Nó 2 (Field Node)**.
- [ ] **Passo 3.2:** Acompanhar o boot do Nó 2 no terminal serial.

**Pontos de Observação (Fase 3):**
- **Log Field Node:** Deve carregar as configurações de rede (v2) e o onboarding (v1) corretamente da NVS.
- **Log Field Node:** Deve logar `Restaurando perfil: Field Node` e iniciar no estado `ACTIVE` (não `PENDING`).
- O Gateway deve restabelecer a comunicação com o Nó 2 rapidamente.

### Fase 4: Resiliência de Transporte (Failover)

*(Requer suporte físico a múltiplos transportes, ex: Wi-Fi + RS485)*
- [ ] **Passo 4.1:** Derrubar propositalmente a rede Wi-Fi primária (ou desconectar a antena).
- [ ] **Passo 4.2:** Observar os logs do Gateway e do Field Node.

**Pontos de Observação (Fase 4):**
- O nó deve mudar o estado de rede para `DEGRADED`.
- O tráfego de métricas/cluster deve chavear para a rede secundária (ex: RS485).
- A métrica `failover_count` em `/api/network/metrics` deve incrementar.
- Restaurar o Wi-Fi e verificar se, após o tempo de hysteresis (ex: 15s), a rede volta a `CONNECTED` e o tráfego retorna para a interface primária.

### Fase 5: Factory Reset

- [ ] **Passo 5.1:** Disparar o Factory Reset no **Nó 2 (Field Node)**. Pode ser via API local no nó 2: `POST /api/system/factory-reset` ou via botão físico, se configurado.
- [ ] **Passo 5.2:** O nó deve reiniciar e voltar ao estado de fábrica.

**Pontos de Observação (Fase 5):**
- **Log Field Node:** `Iniciando Factory Reset Completo...` seguido de apagamento das namespaces `onboarding` e `dev_profile`.
- **Após Boot:** Deve exibir `Nó em estado PENDING, aguardando adoção` e perfil default `Field Node`.

### Fase 6: Observabilidade do Kernel (Determinismo)

Durante **todas** as fases acima, observar continuamente os logs ou métricas do sistema:
- [ ] **Overruns:** O `control_loop_task` (1 ms) logou algum aviso de deadline miss ou overrun? *(Não deve ocorrer, exceto talvez um transiente no instante exato do reset).*
- [ ] **Jitter:** Observar `/api/network/metrics` -> `max_jitter_ms`.

---

## 🚀 Ferramentas de Suporte (Scripts)

Foi criado um script básico para acelerar testes de API (descoberta, métricas e reset):
**Arquivo:** `tools/test_api.sh`

### Uso do Script

1. Torne o script executável: `chmod +x tools/test_api.sh`
2. Testar métricas: `./tools/test_api.sh metrics <IP_DO_NO>`
3. Testar reset local de fábrica: `./tools/test_api.sh reset <IP_DO_NO>`
4. Testar verificação de nós: `./tools/test_api.sh nodes <IP_DO_GATEWAY>`

---

## 🔒 Afirmação Arquitetural

**NADA** no kernel determinístico foi alterado para estas features. Todas as adições de endpoints e NVS ocorrem na _Runtime Layer_ (Service / HTTP) ou durante o _Init_ inicial, mantendo o hot-path do loop de controle livre de bloqueios ou alocações dinâmicas.

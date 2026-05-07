# ENDAP API Contract — referência para o ENDAP Studio

## Objetivo

Este documento define o contrato conceitual inicial entre o firmware ENDAP e o futuro ENDAP Studio.

O objetivo é permitir:

- integração futura via REST/WebSocket;
- desacoplamento gradual da dashboard embarcada;
- mock local do Studio;
- preservação da lógica crítica do firmware;
- evolução do Studio sem inflar o ESP32.

---

# Observações gerais

- O firmware continua sendo a fonte operacional principal.
- O Studio deve atuar como configurador/comissionador/diagnóstico.
- Endpoints abaixo representam rotas identificadas conceitualmente e rotas recomendadas para estabilização futura.
- Algumas rotas já existem parcial ou totalmente na dashboard embarcada.
- Algumas rotas abaixo são propostas para normalização futura.

---

# Contrato REST recomendado

## System

### GET /api/system/info

Finalidade:

Retornar informações gerais do gateway.

Resposta esperada:

```json
{
  "name": "ENDAP Gateway",
  "firmware_version": "0.9.x",
  "uptime": 123456,
  "transport": "ethernet",
  "node_type": "gateway",
  "heap_free": 182344,
  "status": "online"
}
```

Observações:

- Pode alimentar a tela Visão Geral.
- Não deve expor dados sensíveis.

---

## Diagnostics

### GET /api/diagnostics

Finalidade:

Retornar métricas operacionais e diagnóstico.

Resposta esperada:

```json
{
  "heap_free": 182344,
  "heap_min": 160000,
  "cycle_time_us": 4000,
  "jitter_us": 12,
  "overruns": 0,
  "fieldbus_latency_ms": 4,
  "recent_errors": []
}
```

---

## Nodes / Cluster

### GET /api/nodes

Finalidade:

Listar nós conhecidos pelo gateway.

Resposta esperada:

```json
[
  {
    "id": "node-01",
    "alias": "Field Node A",
    "status": "online",
    "transport": "wifi",
    "approved": true,
    "last_heartbeat": 123456
  }
]
```

---

### POST /api/nodes/:id/approve

Finalidade:

Aprovar nó pendente.

Payload esperado:

```json
{
  "profile": "field_node"
}
```

---

### POST /api/nodes/:id/revoke

Finalidade:

Revogar/desativar nó.

---

## IO

### GET /api/io

Finalidade:

Retornar entradas e saídas.

Resposta esperada:

```json
{
  "inputs": [],
  "outputs": []
}
```

---

### POST /api/io/save

Finalidade:

Salvar aliases/configuração de IO.

Payload esperado:

```json
{
  "points": []
}
```

Observações:

- GPIOs reservados devem ser protegidos.
- Studio deve bloquear edição visual de GPIO crítico.

---

## Fail-safe

### GET /api/failsafe/outputs

Finalidade:

Retornar políticas fail-safe.

Resposta esperada:

```json
[
  {
    "output": 1,
    "enabled": true,
    "boot_action": "FORCE_OFF",
    "comm_loss_action": "SAFE_VALUE",
    "runtime_fault_action": "FORCE_OFF",
    "safe_value": 0,
    "recovery_mode": "RESTORE",
    "failsafe_active": false
  }
]
```

---

### POST /api/failsafe/output/save

Finalidade:

Salvar política fail-safe.

---

### POST /api/failsafe/output/reset

Finalidade:

Rearmar/resetar política fail-safe.

---

## Automation

### GET /api/automation/rules

Finalidade:

Retornar regras de automação.

Resposta esperada:

```json
[
  {
    "id": "rule-01",
    "mode": "FOLLOW",
    "enabled": true
  }
]
```

---

### POST /api/automation/rules/save

Finalidade:

Salvar regra.

---

## Logs / Alerts

### GET /api/logs

Finalidade:

Retornar logs recentes e alertas.

Resposta esperada:

```json
[
  {
    "severity": "warning",
    "message": "Node offline",
    "timestamp": 123456
  }
]
```

---

## Backup

### GET /api/backup/export

Finalidade:

Exportar snapshot/configuração do projeto.

---

### POST /api/backup/import

Finalidade:

Importar snapshot/configuração.

---

# WebSocket recomendado

## WS /api/events

Finalidade:

Canal em tempo real para:

- mudanças de IO;
- heartbeat;
- alertas;
- logs;
- alterações de estado;
- eventos fail-safe;
- eventos de cluster.

Mensagem exemplo:

```json
{
  "event": "node_status",
  "node": "node-02",
  "status": "offline"
}
```

---

# Segurança e autenticação

Observações:

- A dashboard embarcada já trabalha com sessão/autenticação local.
- O Studio deve respeitar autenticação existente.
- Não expor endpoints críticos sem autenticação.
- Não permitir escrita irrestrita em IO/fail-safe.
- Operações destrutivas devem exigir confirmação.

---

# Limitações esperadas do firmware

- Recursos HTTP precisam continuar leves.
- Não transformar o ESP32 em frontend pesado.
- Histórico persistente longo deve ficar no Studio.
- Visualização rica e dashboards avançados devem migrar para o Studio.

---

# Estratégia recomendada para o Studio

## Fase V0

- mock local;
- REST simulado;
- backup local;
- shell PWA;
- visual técnico profissional.

## Fase futura

- integração REST real;
- eventos WebSocket;
- sincronização de projeto;
- diagnóstico avançado;
- comissionamento completo;
- editor visual evolutivo.

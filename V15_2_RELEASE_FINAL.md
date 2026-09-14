# ENDAP V15.2 — RELATÓRIO OFICIAL DE RELEASE FINAL

**Data de Homologação**: 23 de Agosto de 2026  
**Status da Versão**: **RELEASE FINAL / FROZEN**  
**Ambiente Alvo**: ESP32-WROOM-32 / ESP32-D0WD-V3 (Dual Core 240 MHz)

---

## 1. Assinaturas Criptográficas e Integridade de Artefatos

Todos os artefatos da versão V15.2 foram auditados e validados bit a bit entre o código-fonte canônico, o recurso binário embarcado no ELF e o payload servido em tempo real pelo microcontrolador.

| Artefato | Assinatura SHA-256 | Status de Correspondência |
| :--- | :--- | :---: |
| **SOURCE_HTML_SHA256** | `a132023b6b1c450dec292a1aad7676a6cdf859bc7fdfc622fb98edf40bad03ac` | **PASS** (100% Bit Match) |
| **SOURCE_GZIP_SHA256** | `6850c53bae08c7c0479e765dba6a77eae48df1de10b08adbe8a5c6e5f5366b13` | **PASS** (100% Bit Match) |
| **EMBEDDED_GZIP_SHA256** | `6850c53bae08c7c0479e765dba6a77eae48df1de10b08adbe8a5c6e5f5366b13` | **PASS** (100% Bit Match) |
| **EMBEDDED_HTML_SHA256** | `a132023b6b1c450dec292a1aad7676a6cdf859bc7fdfc622fb98edf40bad03ac` | **PASS** (100% Bit Match) |
| **ENDAP_BIN_SHA256** | `0f6163679f8546bb0bcf578af786601fa5de31bba5c2db98aaeb7e68ed14ba34` | **PASS** (Binário Oficial) |

- **Tamanho do Binário da Aplicação (`ENDAP.bin`)**: 1.451.568 bytes (23% livre na partição)
- **Tamanho do Dashboard Compactado Embarcado**: 75.111 bytes (Offset ELF: `0x3f42df9c`)

---

## 2. Resultados dos Gates de Homologação

```text
============================================================
MATRIZ DE GATES OFICIAIS — ENDAP V15.2

  SOURCE CANONICAL GATE   : PASS
  GZIP CONTENT MATCH      : PASS
  EMBEDDED RESOURCE GATE  : PASS
  BUILD COMPILATION       : PASS (ESP-IDF Ninja build)
  FLASH TARGET (ESP32)    : PASS (/dev/ttyUSB1 @ 460800 baud)
  BOOT & INITIALIZATION   : PASS (Normal RTS reset, sem watchdog/panics)
  WIFI CONNECTIVITY (STA) : PASS (Conectado SSID: "Rossi", -41 dBm)
  MDNS RESOLUTION         : PASS (endap.local ativo)
  HTTP SERVER (PORT 80)   : PASS (Rotas /, /dash, /index.html)
  ENDAP.LOCAL ACCESS      : PASS (Servindo payload oficial)
  RUNTIME STABILITY       : PASS (Loop 1 ms, Jitter < 15 µs, Overruns = 0)
  API CONTRACTS INTEGRITY : PASS (/api/status, /api/wifi, /api/nodes, /api/automation)
  SECURITY CONTROLS       : PASS (Ações destrutivas com modais de confirmação)
  VISUAL EXPERIENCE GATE  : PASS (5 abas operacionais 100% integradas)
  RESPONSIVENESS GATE     : PASS (Desktop 1369, 1920 e Mobile 390)
============================================================
```

---

## 3. Homologação das Cinco Áreas Operacionais

1. **HOME (Operação)**:
   - Foco e protagonismo nos atuadores e saídas (`● LIGADO` / `○ DESLIGADO`).
   - Leituras ambientais I2C/OneWire em tempo real.
   - Barra de status sintetizada (IP, Uptime, Wi-Fi).
2. **AUTOMAÇÃO**:
   - Lógica de causa e efeito clara (`SE ... ENTÃO ...`).
   - Contador de regras dinâmico (`2 de 16 regras utilizadas`).
   - Intertravamento determinístico no motor local em C.
3. **EQUIPAMENTOS**:
   - Comissionamento de Entradas Digitais, Sensores e Expansores.
   - Catálogo de nós e módulos do cluster.
   - Configuração sob demanda sem ruído visual.
4. **REDE**:
   - Gestão protagonista de conectividade Wi-Fi (SSID, IP, RSSI).
   - Diagnóstico físico de enlaces RS485 Fieldbus e Ethernet RJ45.
   - Ferramentas de contingência e Recovery AP protegidas.
5. **SISTEMA**:
   - Painel de saúde da plataforma e métricas de execução do kernel de 1 ms.
   - Pipeline sequencial de 5 fases determinísticas sobre o budget de 1000 µs.
   - Alertas, auditoria local em RAM (max 32 eventos) e RBAC persistido em NVS.

---

## 4. Resoluções e Viewports Homologados

- **1369 × 768 (Desktop Standard / Painel Industrial)**: **PASS** (Zero scroll horizontal, layout simétrico).
- **1920 × 1080 (Desktop Full HD)**: **PASS** (Nitidez, legibilidade e alinhamento).
- **390 × 844 (Mobile / Tablet de Campo)**: **PASS** (Cards responsivos empilhados, alvos de toque > 44px).

---

## 5. Auditoria de Apontamentos de Release

- **P0 (Bloqueadores de Release)**: **0** (Nenhum)
- **P1 (Problemas Funcionais / Contratos)**: **0** (Nenhum)
- **P2 (Problemas Visuais Importantes)**: **0** (Nenhum)
- **P3 (Cosméticos Não-Bloqueantes)**: **2**
  - *P3.1*: Padronização futura de ícones Unicode remanescentes nas legendas das micro-leituras ambientais.
  - *P3.2*: Nomenclatura do payload JSON exportado pelo botão de relatório técnico.

---

## 6. Hotfixes Críticos Preservados

1. **`global_json_api_buffer`**: Buffer JSON global protegido em [`components/runtime/http_server/http_server.c`](file:///home/mateus/dev/github/endap-firmware/components/runtime/http_server/http_server.c) para evitar alocações dinâmicas no heap e prevenir fragmentação.
2. **`esp_wifi_set_max_tx_power(52)`**: Calibração de potência de rádio em [`components/runtime/network/wifi_manager/wifi_manager.c`](file:///home/mateus/dev/github/endap-firmware/components/runtime/network/wifi_manager/wifi_manager.c) garantindo estabilidade de RF sem reset por brownout.

---

## 7. Arquivos da Release V15.2

- [`components/dashboard/index.html`](file:///home/mateus/dev/github/endap-firmware/components/dashboard/index.html) — Código-fonte canônico unificado (3.505 linhas).
- [`components/dashboard/index.html.gz`](file:///home/mateus/dev/github/endap-firmware/components/dashboard/index.html.gz) — Binário compactado embarcado no firmware (75.111 bytes).

---

## 8. Declaração Obrigatória de Baseline

> **"ENDAP V15.2 é o baseline oficial homologado em hardware real.**
>
> **A partir deste momento, nenhuma alteração de UI, HTML, CSS ou layout deve ser aplicada sobre a V15.2.**
>
> **Qualquer expansão ou alteração visual futura deverá iniciar uma nova versão formal."**

---

```text
STATUS FINAL:
  ENDAP V15.2
  RELEASE GATE = PASS
  DESIGN = FROZEN
  RUNTIME = PASS
  FIRMWARE = VALIDATED
  FLASH = VALIDATED
```

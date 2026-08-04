# ENDAP

**Edge-Native Distributed Automation Platform**

ENDAP is an embedded automation platform for ESP32-class nodes designed around deterministic local execution, guided node onboarding, modular communication, and incremental evolution toward a professional distributed automation system.

The project combines:
- a deterministic local runtime
- product-oriented gateway/node architecture
- simple operator adoption flow
- a modular path toward distributed coordination

---

## Core Vision

ENDAP is being built as a **platform-first automation system**, not as a collection of isolated ESP devices.

The same conceptual base should scale from:
- a single simple node
- a gateway plus a few field nodes
- a larger distributed deployment with multiple nodes and richer communication layers

Current product direction prioritizes:
- local-first operation
- simple adoption
- deterministic local behavior
- gradual hardware validation
- future scalability without architectural rupture

---

## Current Active Scope

The current implementation focus is **ENDAP v1 as a usable product baseline**.

Active scope:
- Onboarding v1
- Network v1
- Gateway v1
- Field Node v1
- Dashboard v1 for adoption and operation
- Node profiles v1
- Templates v1
- Basic cloning/replication
- Minimal persistence
- Basic recovery/reconnection
- Kernel Product Freeze v1

Out of immediate scope:
- LoRa
- full Wi-Fi Mesh productization
- cloud-first features
- full Studio
- advanced distributed automation
- broad multi-transport orchestration in production

---

## Architecture

Main layers:

- **Kernel** — deterministic and critical
- **Runtime** — local execution, state, automation and IO coordination
- **Services** — network, onboarding, dashboard, registry, APIs
- **UI** — embedded dashboard, future Studio alignment

Separation rule:
- the kernel must remain protected from service-layer coupling
- product features should be added outside the hot path whenever possible

---

## Runtime Core

Control loop:
- `1 ms` deterministic cycle driven by GPTimer

Scheduler phases:
- `IO -> Fieldbus -> Automation -> Events -> Diagnostics`

Core observability:
- determinism probe
- kernel trace
- phase-level timing metrics
- watchdog supervision
- runtime/kernel metrics

---

## Implemented / Consolidated Base

The codebase already includes a substantial embedded runtime foundation, including:

- deterministic 1 ms control loop
- phased scheduler
- state handling and IO integration
- observability for jitter, execution time and overrun analysis
- HTTP/dashboard integration through safe paths
- RS485 fieldbus baseline
- cluster baseline primitives
- persisted automation rules
- snapshot-based restore foundations
- operator-facing dashboard surface

This means the current project phase is not about starting from zero, but about consolidating the base into a **minimal coherent product flow**.

---

## Product-Oriented Direction

The platform is evolving around the following product concepts:

- **Gateway** as the main communication and coordination node
- **Field Nodes** as focused distributed nodes
- **Node profiles** to simplify deployment
- **Templates** to accelerate adoption
- **Node registry** for discovery/adoption lifecycle
- **Bootstrap flow** for simple commissioning
- **Selective transport enablement** by node role

Initial node profile direction includes:
- Gateway
- Field Node
- Relay Node
- Sensor Node
- Local I/O Node

---

## Communication Direction

ENDAP is designed to support multiple communication layers over time, but only the required transports should be enabled per node.

Relevant current transports:
- Wi-Fi
- RS485
- Ethernet/RJ45 preparation
- common transport contract for evolution

Important architectural rule:
- not every transport should be active by default
- drivers/services must be initialized only when required by the node role/configuration

---

## Observability

Current observability direction includes:

- jitter metrics (`min / avg / max`)
- execution time tracking
- phase timing
- overrun and deadline monitoring
- fieldbus latency / retry / timeout visibility
- node and transport status visibility
- product-facing diagnostics through the dashboard

Observability is treated as a first-class capability for:
- debugging
- product hardening
- commissioning
- TCC evidence

---

## Hardware / Stack

Current target:
- ESP32-WROOM-32

Technology stack:
- ESP32
- Xtensa LX6
- FreeRTOS
- ESP-IDF `v5.1.2`

Planned future evolution:
- ESP32-S3
- broader HAL-oriented portability

---

## Current Project Status

Current project state can be summarized as:

### Stable foundation already available
- kernel and scheduler base
- deterministic runtime core
- observability foundation
- dashboard/API foundation
- automation persistence baseline
- fieldbus baseline
- cluster baseline

### Current Project Phase (V1 Product Freeze)
- **Product-oriented v1 flow**: COMPLETED
- **Onboarding and basic operation**: COMPLETED
- **Gateway ↔ Node behavior**: COMPLETED
- **Determinism and UX alignment**: COMPLETED
- **Hardware-oriented validation path**: IN PROGRESS (Current Focus)

### Immediate validation direction
1. Wi-Fi common workflow
2. basic cluster behavior
3. RS485 real validation
4. Ethernet/RJ45 validation
5. later mesh-oriented evolution, if needed

---

## Main References

- `docs/kernel.md`
- `docs/fieldbus.md`
- `docs/architecture.md`
- `docs/current_state.md`
- `ROADMAP.md`

---

## 🧪 Protocolo de Testes de Bancada (Bench Test Protocol)

### [TESTE 1: DETERMINISMO E JITTER DO LOOP DE 1 MS]
* **Objetivo:** Provar que o loop de controle não estoura a janela de 1000 µs.
* **Procedimento:** Executar o nó por 10 minutos realizando leituras e escritas de E/S.
* **Métrica de Sucesso:** `max_jitter_us` < 50 µs e `deadline_miss_count` == 0.

### [TESTE 2: RESILIÊNCIA E TEMPO DE SAFE_STATE]
* **Objetivo:** Medir o tempo de resposta do acionamento autônomo de segurança ao desconectar o barramento RS-485.
* **Procedimento:** Conectar o Nó ao Gateway Master com irrigação simulada LIGADA e desengatar fisicamente a linha RS-485.
* **Métrica de Sucesso:** O canal de irrigação deve DESLIGAR em exatamente **500 ms** (`FIELDBUS_TIMEOUT_MS`) e registrar o evento de `SAFE_STATE` na auditoria.

### [TESTE 3: ISOLAMENTO E IMUNIDADE A RUÍDO]
* **Objetivo:** Garantir que o chaveamento indutivo não cause resets nem estáticos no ESP32.
* **Procedimento:** Comutar solenoides de 24VDC 100 vezes consecutivas.
* **Métrica de Sucesso:** 0 resets por Watchdog e 0 erros de leitura nas entradas optoacopladas.

---

## 🎓 Roteiro de Apresentação TCC / Pitch Executivo

1. **Slide 1: Capa** — ENDAP: Automação Distribuída de Borda para Agricultura de Precisão.
2. **Slide 2: O Problema** — Risco de perda de safras por travamentos em sistemas amadores vs. CLPs caros de R$ 50 mil.
3. **Slide 3: A Solução ENDAP** — Arquitetura distribuída local-first com determinismo de 1 ms e hardware blindado.
4. **Slide 4: Leis Arquiteturais** — Zero Malloc, Failsafe < 500 ms, execução isolada no Core 1.
5. **Slide 5: Hardware Blindado** — Optoacoplamento PC817, Diodos TVS SM712 e Proteção Indutiva para Solenoides.
6. **Slide 6: Software & Determinismo** — Pipeline em 5 fases e medições empíricas de jitter.
7. **Slide 7: Demonstração ao Vivo** — Dashboard embarcada e transição autônoma para `SAFE_STATE`.
8. **Slide 8: Conclusão & Evolução** — Expansão da plataforma e produto comercial v1.0.

---

## Development Philosophy

**Simple, deterministic, robust, usable, and incrementally scalable.**

---

## Author

Mateus Rossi


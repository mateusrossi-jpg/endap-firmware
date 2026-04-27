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

### Current implementation priority
- close product-oriented v1 flow
- improve onboarding and operation
- validate gateway ↔ node behavior
- preserve determinism while increasing usability
- finish hardware-oriented validation path

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

## Development Philosophy

**Simple, deterministic, robust, usable, and incrementally scalable.**

---

## Author

Mateus Rossi

# ENDAP Roadmap

This roadmap reflects the **current official implementation scope** of ENDAP.

The project already has a substantial technical base. The present goal is to transform that base into a **usable product-oriented v1**, without breaking determinism and without expanding scope irresponsibly.

---

## Phase 1 — Kernel Foundation
**Status:** `done`

Consolidated base:
- 1 ms deterministic control loop
- phase scheduler
- determinism probe
- kernel trace
- watchdog integration
- phase-level timing export
- stable separation between critical execution and non-critical services

Notes:
- this phase is considered the technical foundation of the platform
- kernel changes from this point onward should be minimal, explicit and strongly justified

---

## Phase 2 — Runtime Stabilization and Product Freeze
**Status:** `done / protected`

Consolidated direction:
- local execution core stabilized
- IO integration structured around deterministic execution
- instrumentation available for timing analysis
- critical runtime path already treated as protected base

Current rule:
- the goal is no longer to expand the kernel aggressively
- the goal is to preserve and only adjust when necessary for product closure

---

## Phase 3 — Fieldbus Core
**Status:** `implemented baseline, real hardware validation pending`

Scope already present:
- RS485 master polling baseline
- frame integrity with `CRC16`
- retry and timeout accounting
- node status tracking
- bus latency visibility
- bench-oriented validation path

Next practical work:
- validate RS485 with two real boards
- confirm behavior under real timing and wiring conditions
- keep fieldbus evolution compatible with product v1 scope

---

## Phase 4 — Cluster / Network Baseline
**Status:** `implemented baseline, incremental product integration in progress`

Implemented baseline:
- node identity
- cluster manager foundations
- ownership / failover primitives
- dashboard-level visibility hooks

Current direction:
- treat networking as part of **Network v1**
- keep complexity controlled
- validate gateway ↔ peer behavior incrementally
- avoid over-expanding failover logic beyond current practical needs

Important note:
- cluster exists as a baseline capability, but the active implementation priority is the usable product flow, not advanced distributed behavior

---

## Phase 5 — Automation Engine
**Status:** `implemented baseline`

Available foundation:
- persisted rules in NVS
- restore on boot
- rule evaluation baseline
- dashboard-side rule authoring support

Available modes:
- `FOLLOW`
- `PULSE_MS`
- `ON_DELAY_MS`
- `OFF_DELAY_MS`
- `TOGGLE`
- `FORCE_ON`
- `FORCE_OFF`

Current direction:
- preserve what already works
- improve usability only where needed
- avoid deep expansion of automation complexity during product v1 closure

---

## Phase 6 — Onboarding v1
**Status:** `active priority`

Target:
build the first coherent adoption flow for ENDAP nodes.

Main blocks:
- bootstrap identity for nodes
- gateway-side node registry
- discovery / adoption flow
- profile assignment
- template application
- IO test / confirmation
- activation flow
- minimal persistence
- basic recovery on reboot/reconnect

Goal:
make ENDAP behave like a platform that receives and activates nodes, not merely a set of isolated ESP firmwares.

---

## Phase 7 — Gateway v1
**Status:** `active priority`

Target:
close the first usable gateway role in product terms.

Expected responsibilities:
- discovery and adoption entry point
- node registry ownership
- dashboard hosting
- configuration entry point
- transport/service activation according to role
- operational visibility for nodes and IO
- basic commissioning flow

Rule:
the gateway is primarily a communication and coordination node, not a place to dump unnecessary critical logic.

---

## Phase 8 — Field Node v1
**Status:** `active priority`

Target:
define and validate the first practical product-facing distributed node behavior.

Expected blocks:
- bootstrap/startup identity
- association with gateway
- local IO operation
- heartbeat / status reporting
- profile-driven behavior
- persistence of essential identity/config
- basic recovery / reconnection

Goal:
support real expansion of the system using simple, focused nodes.

---

## Phase 9 — Dashboard v1
**Status:** `in progress`

Target:
make the embedded dashboard useful for both adoption and operation.

Current direction:
- discovery/adoption surface
- node state visibility
- IO visibility/control where appropriate
- automation usability improvements
- diagnostics that are immediately actionable
- simpler operator experience

Rule:
dashboard work must not compromise kernel determinism.

---

## Phase 10 — Profiles, Templates and Basic Replication
**Status:** `next active track`

Target:
reduce commissioning friction.

Includes:
- initial node profile set
- template application flow
- parameter presets
- repeatable configuration patterns
- basic cloning / replication for new nodes

Goal:
accelerate deployment without introducing excessive architecture complexity.

---

## Phase 11 — Validation and Hardening
**Status:** `next`

Practical validation sequence:
1. validate common Wi-Fi workflow
2. validate basic cluster/network behavior
3. validate RS485 on real hardware
4. validate Ethernet/RJ45 path
5. only later evaluate mesh-oriented expansion if still justified

Hardening goals:
- eliminate regressions
- preserve determinism under service load
- improve commissioning reliability
- produce a stable demonstrable build

---

## Deferred / Not Now

These items remain part of the broader ENDAP vision, but are **not** part of the immediate implementation scope:

- LoRa
- Wi-Fi Mesh as mandatory architecture
- full Studio
- cloud monetization layer
- advanced redundancy
- dual-MCU architecture
- full multi-transport production orchestration
- complex distributed automation graph
- full commercial hardware family rollout

---

## Strategic Rule

The roadmap should be interpreted with one permanent constraint:

**Do not sacrifice determinism, architectural clarity, or implementation focus in exchange for premature feature expansion.**

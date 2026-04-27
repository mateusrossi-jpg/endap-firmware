# ENDAP Architecture

ENDAP is structured as a layered runtime with deterministic execution at the center and operator tooling on top.

## Operator Layer

- Dashboard at `/dash`
- HTTP / WebSocket status surface
- Captive portal and Wi-Fi provisioning

This layer exists for operation, diagnostics and configuration. It should never become the timing reference for the control loop.

## Application Layer

- Automation engine
- Rule persistence
- Output command semantics

This layer expresses system behavior in terms of rules and desired output state.

## Deterministic Kernel

- 1 ms GPTimer-driven control loop
- phase scheduler
- watchdog integration
- determinism probe
- kernel and phase metrics

This is the timing-critical core of ENDAP.

## Runtime Services

- IO driver
- RS485 engine / master
- snapshot persistence
- bus health monitor
- HTTP server
- Wi-Fi manager

These services support the kernel and operator surface while keeping heavy work away from the timing-critical path whenever possible.

## Cluster Layer

- node identity
- cluster manager
- cluster IO ownership
- failover / failback
- discovery hooks

This layer coordinates ownership and health across peers.

## HAL / Platform

- GPIO
- UART / RS485 transceiver
- GPTimer
- Wi-Fi
- ESP-IDF / FreeRTOS platform services

## Current Architectural Notes

- Snapshot save work is processed outside the critical deterministic path.
- RS485 self-test is used to validate fieldbus behavior with one ESP32 on the bench.
- Cluster self-test logic exists, but is currently disabled by default to avoid unintended output activity in the normal build.

## Stable v1 Phase 1 Context Model

- Each node exposes its local contextual catalog through the profile API, including capabilities, component states, channel inventory and binding backends.
- Effective capability is resolved as `hardware -> profile -> active_config`; the dashboard must show this as context, not as a manual guess.
- The gateway node registry is the source of truth for remote node admission, profile label and operational context shown in `/api/nodes`.
- The main operational API should keep exposing active runtime state, while disabled or future-capable components remain visible only in the contextual/catalog view.
- This model lives in services/runtime API code and must not add work to the 1 ms deterministic control loop.

## Stable v1 Phase 2 Security And Adoption Model

- The official local roles are `admin`, `integrator`, `operator` and `viewer`; `installer` remains accepted as a compatibility alias for `integrator`.
- `admin` owns account management and every critical action.
- `integrator` can configure transport/profile, admit nodes, operate IO, manage automation and run recovery, but cannot manage security accounts.
- `operator` can read the dashboard and operate manual IO.
- `viewer` can read the dashboard only.
- Node adoption remains explicit: discovered nodes must be approved/configured/activated before participating as operational nodes, and revocation returns them to a pending/discovered state with audit.

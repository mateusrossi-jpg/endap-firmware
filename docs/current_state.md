# ENDAP Current State

Last updated: `2026-03-29`

## Bench-Validated Today

- 1 ms deterministic kernel running stably on ESP32
- dashboard with tabs and operator-style monitoring surface
- live diagnostics for loop, bus and inputs
- live performance graph in monitoring
- persistent automation rules in NVS
- snapshot persistence for output state restore
- RS485 self-test for single-board bench validation

## Implemented Core Features

### Automation

- persisted rules
- simple and advanced editing
- rule conflict control by output
- modes:
  - `FOLLOW`
  - `PULSE_MS`
  - `ON_DELAY_MS`
  - `OFF_DELAY_MS`
  - `TOGGLE`
  - `FORCE_ON`
  - `FORCE_OFF`

### Fieldbus

- RS485 master/engine
- CRC validation
- retries and timeout accounting
- latency metrics
- dashboard health mapping

### Cluster

- node identity
- cluster registry / manager
- IO ownership
- failover / failback

Current caveat:
- cluster self-test is intentionally disabled by default in the current build to avoid unsolicited relay activity during normal bench use

### Dashboard

- tabs for `Monitoring`, `Automation`, `IO`, `Cluster`
- hero summary
- operational state
- diagnostics cards
- live alerts
- live trend graph
- automation builder with presets and preview

## Known Pending Validation

- real RS485 communication with two physical ESP32 nodes
- real multi-node cluster validation on Wi-Fi STA
- broader regression test coverage

## Current Product Focus

- keep refining dashboard usability
- simplify automation authoring
- preserve deterministic behavior while improving operator visibility

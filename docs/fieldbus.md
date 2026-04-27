# ENDAP Fieldbus

The ENDAP fieldbus layer is based on deterministic RS485 communication.

## Current Capabilities

- master polling model
- validated wire framing
- `CRC16` integrity checking
- retry and timeout accounting
- per-window latency metrics
- node online/offline tracking

## Bench Workflow

The current default bench build keeps RS485 self-test enabled.

What that means:
- the master still emits normal `POLL` flow
- the engine injects a synthetic `ACK` for `node 1`
- latency, parser, retry logic and diagnostics are exercised with a single ESP32

This lets ENDAP validate most of the fieldbus path without requiring a second board during early development.

## Two-Board Validation

For real hardware validation, the repository includes a minimal slave app:
- [rs485_slave_test/README.md](/home/mateus/dev/projects/ENDAP/rs485_slave_test/README.md)

That app answers the master with a real `ACK` over RS485 using a second ESP32.

## Observability

Fieldbus health is exported to the dashboard and HTTP status with:
- retries
- timeouts
- CRC errors
- average latency
- max latency
- self-test flag

## Remaining Work

- validate multi-board wiring and behavior end to end
- expand beyond the current bench `node 1` path
- validate behavior with real cluster traffic running at the same time

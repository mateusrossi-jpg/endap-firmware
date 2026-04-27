# ENDAP Kernel

The ENDAP kernel is a deterministic runtime for industrial-style automation workloads on ESP32.

## Control Loop

Cycle time:
- `1 ms`

Scheduler phases:
1. IO
2. IO Apply
3. Fieldbus
4. Automation
5. Events
6. Diagnostics

The control loop is driven by GPTimer and instrumented continuously.

## Monitoring

The kernel exports:
- determinism probe
- kernel trace
- deadline miss count
- overrun count
- per-phase maximum execution windows
- per-phase overrun counters and configured phase deadlines

These metrics are visible in logs and on the dashboard diagnostics surface.

## Validation Mode

For Kernel V1 validation, the runtime exposes a controlled per-phase load injector.

- It is disabled by default.
- It injects bounded busy-wait time into a selected phase.
- It is intended for bench validation of warnings, overruns and exported telemetry.
- It is available through `/api/kernel/load` and surfaced on the dashboard diagnostics card.

## Current Execution Profile

In healthy bench operation, the runtime typically shows:
- low-tens-of-microseconds jitter windows
- sub-`100 us` average execution windows
- higher transient peaks when Wi-Fi, HTTP or snapshot activity overlaps

Those peaks are surfaced as diagnostics instead of being hidden.

## Determinism Notes

- IO scan remains in the deterministic path.
- Snapshot save processing was moved out of the heavy critical path.
- Input diagnostics run in background reporting rather than inside the 1 ms loop.
- The dashboard consumes exported metrics, but does not drive kernel timing.
- The software watchdog is supervised off-loop by the auxiliary task, while the
  control loop only feeds it after a completed cycle.
- Public loop metrics are exported through a stable `control_loop_get_metrics()`
  API backed by the kernel metrics snapshot.

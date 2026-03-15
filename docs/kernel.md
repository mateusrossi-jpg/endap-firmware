# ENDAP Kernel

The ENDAP kernel implements a deterministic control runtime designed
for industrial automation workloads.

## Control Loop

Cycle time: **1 ms**

Scheduler phases:

1. IO
2. Fieldbus
3. Automation
4. Events
5. Diagnostics

The control loop is driven by hardware GPTimer to ensure deterministic
timing.

## Determinism Monitoring

The runtime includes:

- determinism probe
- kernel trace
- deadline monitor
- watchdog supervision

## Execution Metrics

Typical runtime metrics:

jitter(avg) ≈ 28 µs  
execution(max) ≈ 188 µs

The system maintains ~80% cycle slack.

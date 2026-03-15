# ENDAP Fieldbus

The ENDAP fieldbus layer is based on deterministic RS485 communication.

## Characteristics

- Master polling model
- Deterministic request scheduling
- Bus health monitoring

## Latency

Typical measured latency:

<100 µs: 100%

## Future Work

The fieldbus layer will evolve into the ENDAP Fieldbus Protocol with:

- CRC16 integrity checks
- message sequencing
- retry policies
- automatic baud detection

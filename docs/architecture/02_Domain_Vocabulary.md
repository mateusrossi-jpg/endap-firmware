# Domain Vocabulary: ENDAP Runtime

This document defines the 20 fundamental concepts (the meta-model) of the ENDAP Runtime. Every component, variable, and process must map to these definitions.

1.  **Runtime Kernel:** The infrastructure base (Scheduler, Lifecycle/RTO, Boot, Watchdog).
2.  **Runtime Object (RTO):** An entity with a standard lifecycle (Create, Init, Start, Run, Pause, Stop, Destroy).
3.  **Resource:** Shared hardware/software resources (I2C Bus, DMA, Mutex) requiring access arbitration.
4.  **Capability:** Semantic description of a provided feature (ex: `TemperatureMeasurement`).
5.  **Capability Provider:** A contract role (Hardware, Modbus, AI) that implements a Capability.
6.  **Driver:** A pure hardware protocol translator (Hardware → Capability Provider).
7.  **Device:** A logical aggregator of physical/virtual instances.
8.  **Channel:** A semantic attribute of a `Device` exposing a `Capability`.
9.  **Binding:** A declarative pipeline mapping a `Channel` to a `PVE` (Filter, Scale, Conversion).
10. **PVE (Process Variable Entity):** The unique truth of the process (State, History, Metadata).
11. **Registry:** Infrastructure for dynamic indexing (Driver, Device, PVE, Service, Resource).
12. **Event:** A reactive message (`source`, `target`, `type`, `payload`, `timestamp`).
13. **Command:** A request for action with a single responsible target.
14. **Query:** A request for information without side effects.
15. **Service:** An RTO providing integration/support (API, MQTT, Cluster).
16. **Descriptor:** Static metadata for introspection (Studio, UI, API).
17. **Context:** Logical scope/hierarchy (Machine, Plant, Simulation).
18. **Runtime Manifest:** Declarative definition of system topology (Config/Topology).
19. **Policy:** Rules governing access, mutability, and lifecycle.
20. **Ownership:** Governance model (Single Owner, Single Write principle).

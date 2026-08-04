# System Model: ENDAP Runtime

The System Model defines the structural hierarchy of the ENDAP Runtime. It enforces a strict separation between the Kernel (Infrastructure), Runtime (Domain Logic), and Services (Communication/Integration).

## 1. High-Level Hierarchy
The system follows a layered dependency graph:
1.  **Kernel Layer:** (Most inner layer) Infrastructure base.
2.  **Runtime Layer:** (Domain/Services) Business logic and entity management.
3.  **Adaptation Layer:** (Drivers/HAL) Hardware communication.

## 2. Structural Graph (Dependencies)

```text
[ APPLICATION ] (Ladder, Studio)
      │
[ SERVICES ] (API, MQTT, Cluster, Historian)
      │
[ RUNTIME DOMAIN ] (PVE Manager, Automation Engine, Binding)
      │
[ RUNTIME INFRASTRUCTURE ] (Registry, Event Bus, Configuration)
      │
[ KERNEL ] (Scheduler, Lifecycle/RTO, Boot, Memory)
      │
[ HAL / DRIVERS ] (Hardware Abstraction Layer)
```

## 3. Interaction Rules
*   **Layer Sovereignty:** A layer may only depend on layers below it.
*   **Dependency Injection:** Dependencies are injected via Registry/Configuration at boot time, not hardcoded at compile-time.
*   **Event-Driven Communication:** Components interact through the `Event Bus` or well-defined `API` contracts, never direct function calls across layer boundaries.
*   **Capability Flow:** Hardware capabilities are lifted through Drivers to Devices, exposed via Channels, and finally mapped to PVEs.

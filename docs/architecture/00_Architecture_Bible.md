# Architecture Bible: ENDAP Runtime

## Preface
The Architecture Bible defines the normative, immutable principles that govern the ENDAP Runtime. It acts as the "Constitution" of the system. Its purpose is to ensure long-term sustainability, platform portability, and architectural integrity. This document is intended for architects and core developers.

## 1. Vision
ENDAP is a platform-agnostic, process-centric automation runtime. It decouples industrial automation logic from hardware and communication protocols, allowing it to evolve across hardware platforms (ESP32, STM32, Linux Industrial) while maintaining the same stable business logic.

## 2. Non-Goals
ENDAP does **not** aim to be:
*   A general-purpose driver framework.
*   A proprietary RTOS.
*   A database solution.
*   A standalone MQTT/REST middleware.
*   A library for hardware-specific HALs.

## 3. Core Principles (Architectural Laws)
These laws are immutable and mandatory for every component.

1.  **Process-Centric:** The system operates on Process Variables (PVEs). Hardware is exclusively a supplier of data (Capability Provider).
2.  **Single Ownership:** Every architectural entity (PVE, Device, Registry, etc.) has exactly one Owner component responsible for its state.
3.  **Single Write:** All state changes must be requested through the authorized Owner, mediated by Policy.
4.  **Kernel Neutrality:** The Runtime Kernel has zero knowledge of domain concepts (PVE, Device, Automation). It only manages Runtime Objects (RTOs).
5.  **Capability-Oriented Design:** Components interact through abstract Capabilities, not through specific hardware drivers or protocols.
6.  **Domain Isolation:** Infrastructure (Registry, Event Bus) is strictly separated from Domain Logic (PVE Manager, Automation).

## 4. Architectural Invariants
Invariants are system properties that must hold true at all times.

*   **PVE Ownership:** Every PVE must be owned by exactly one Manager.
*   **Write Policy:** No module may modify PVEs, Devices, or Registries without passing through the authorized Manager’s Policy.
*   **Descriptor Integrity:** Every entity must possess a Descriptor if it is to be introspected.
*   **Binding Pipeline:** Every Binding must connect exactly one Provider (via Channel) to exactly one Consumer (via PVE).
*   **Capability Integrity:** Every Capability Provider must implement its declared interface contract.
*   **Lifecycle Determinism:** Every Runtime Object must implement a strict Lifecycle state machine.
*   **Domain Isolation:** No component of the Domain Runtime may include headers of the Hardware Drivers.

## 5. Evolution Protocol
No new architectural concept may be introduced directly into the code. The mandatory evolution path is:

1.  **Ideation/Proposal** (Architecture Bible Update).
2.  **Vocabulary Definition** (Domain Vocabulary Update).
3.  **Governance Definition** (Ownership/Lifecycle Update).
4.  **Architecture Decision Record (ADR)** (Formalizing the decision).
5.  **API Contract** (Definindo a interface).
6.  **Implementation** (Código funcional).
7.  **Architectural Fitness** (Testes de conformidade).
8.  **Release** (Documentação final).

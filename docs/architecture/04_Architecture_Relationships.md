# Architecture Relationships: ENDAP Runtime

## 1. Layer View (Conceptual)
A organização lógica do sistema segue a seguinte hierarquia de abstração:
1.  **Application:** Ladder Logic, Studio, Dashboard.
2.  **Services:** API, MQTT, Cluster, Historian.
3.  **Runtime Domain:** PVE Manager, Device Manager, Automation Engine, Binding Pipeline.
4.  **Runtime Infrastructure:** Registry, Event Bus, Configuration Manager.
5.  **Kernel:** Scheduler, Lifecycle/RTO, Boot, Memory Services.
6.  **Platform Abstraction (PAL):** Abstração de hardware agnóstica (ex: `pal_i2c.h`).
7.  **HAL/Drivers:** Implementações concretas de hardware (ESP32, STM32, Linux).

## 2. Compile-time Dependency Rules
Regras de `#include` obrigatórias para garantir o desacoplamento:

| Origem | Pode depender de (Include/Link) | Nunca deve depender de |
| :--- | :--- | :--- |
| **Application** | Services, API | Domain, Infra, Kernel, HAL |
| **Services** | Domain, Infra, Kernel | Drivers, HAL |
| **Runtime Domain** | Infra, Kernel | Drivers, HAL |
| **Runtime Infra** | Kernel | Domain, Drivers, HAL |
| **Kernel** | Platform Abstraction | Domain, Services, HAL |
| **Drivers** | HAL | Runtime, Services, Domain |

## 3. Dependency Inversion
Módulos de alto nível (Automation, Services) dependem exclusivamente de contratos (APIs). A implementação concreta (Driver) é injetada via Registry no boot.
*   **Allowed:** `Automation` → `Temperature Capability API`.
*   **Forbidden:** `Automation` → `AHT10 Driver`.

## 4. Allowed Communication Mechanisms
Toda interação entre módulos deve utilizar um dos quatro canais:
*   **Function Call:** Apenas dentro do mesmo domínio ou via API pública.
*   **Public API:** Uso de contratos `_api.h`.
*   **Event Bus:** Pub/Sub para reatividade.
*   **Registry Lookup:** Para descoberta de serviços/capacidades.

## 5. Anti-Dependencies (Classification)
*   **Class A (Constitution Breach):** Ex: `Driver` → `PVE` / `Automation` → `HAL`. (Erro fatal)
*   **Class B (Governance Breach):** Ex: `Service` → `internal/`. (Erro grave)
*   **Class C (Directive Breach):** Ex: `Binding` → `Driver concreto`. (Dívida técnica)

## 6. Boundary Crossing
Toda travessia entre domínios (ex: Runtime Infra → Runtime Domain) deve ocorrer exclusivamente por:
*   Capability API
*   Registry API
*   Command / Query interface
*   Event Bus

## 7. Friend Relationships
Relações de exceção que permitem acesso a estruturas `internal/` (explicitamente aprovadas):
*   `Runtime Infra/Registry` ↔ `Runtime Infra/Event Bus` (Integração de baixo nível).
*   `Runtime Domain/PVE` ↔ `Runtime Domain/Device` (Via `internal` API).

## 8. Architectural Matrix & Fitness
| Relação | Permitida | Fitness Function |
| :--- | :---: | :--- |
| Compile Dependency | ✔ | Static Include Analysis |
| Runtime Event | ✔ | Event Bus Monitor |
| Ownership | ✔ | Ownership Matrix Validation |
| Lifecycle | ✔ | State Machine Analysis |
| Visibility | ✔ | Include Checker (API vs Internal) |

## 9. Architectural Smells (Forbidden)
*   **God Manager:** Módulo conhece todos os outros (ex: `TelemetryTask` atual).
*   **Cyclic Dependency:** `A` importa `B`, `B` importa `A`.
*   **Leaky Abstraction:** Driver vazando conceitos de PVE ou Automation.
*   **Shared Mutable State:** Dois Owners para a mesma entidade.
*   **Registry as Database:** Armazenar lógica dentro do Registry.
*   **Fat Driver:** Driver contendo regras de negócio.

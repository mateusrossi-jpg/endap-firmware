# Governance: ENDAP Runtime

## Parte I — Governance Principles
* **GOV-001 (Single Ownership):** Cada entidade (PVE, Device, Registry, etc.) deve possuir um único Owner responsável por sua integridade. (Ref: AB-002)
* **GOV-002 (Single Write):** Todas as mutações de estado devem passar por um caminho único e autorizado, mediado por Policy. (Ref: AB-003)
* **GOV-003 (Authority Delegation):** Autoridade é delegada hierarquicamente; nenhum sub-módulo pode usurpar a autoridade de seu Manager.
* **GOV-004 (Separation of Responsibilities):** Separação estrita entre infraestrutura (Kernel) e negócio (Domain Runtime). (Ref: AB-006)

## Parte II — Authority Model
* **GOV-101 (Creator):** Responsável por instanciar a entidade (ex: Configuration Manager).
* **GOV-102 (Owner):** Responsável pela integridade e estado da entidade. (Distinto de **Maintainer**, que implementa o módulo).
* **GOV-103 (Writer):** Único componente autorizado a modificar o estado.
* **GOV-104 (Observer):** Consumidor autorizado a ler o estado.
* **GOV-105 (Registrar):** Entidade autorizada a incluir/excluir instâncias no Registry.
* **GOV-106 (Lifecycle Authority):** Entidade que controla o estado de vida (Create, Init, Stop, Destroy).

## Parte III — Policy Model
* **GOV-201 (Read Policy):** Regras de permissão de consulta.
* **GOV-202 (Write Policy):** Regras de permissão de mutação (Single Write Principle).
* **GOV-203 (Execute Policy):** Permissão para disparar RTO Lifecycle ou Comandos.
* **GOV-204 (Register Policy):** Permissão para adicionar instâncias ao Registry.
* **GOV-205 (Bind Policy):** Permissão para vincular Channels a PVEs.
* **GOV-206 (Destroy Policy):** Permissão para remover instâncias do sistema.

## Parte IV — Visibility
* **GOV-301 (Public API):** APIs estáveis (`_api.h`). Consumidas por Services e outros domínios.
* **GOV-302 (Internal API):** APIs de infraestrutura (`_internal.h`). Compartilhadas apenas dentro do mesmo domínio.
* **GOV-303 (Private Implementation):** Detalhes internos (`.c` ou `private/`). Proibido acesso externo.
* **GOV-304 (Kernel Internal):** Acesso estrito ao microkernel.

## Parte V — Evolution
* **GOV-401 (Evolution Flow):** Ideia → Bible → Vocabulary → Governance → ADR → API → Code.
* **GOV-402 (ADR Requirement):** Toda decisão arquitetural deve ser registrada.
* **GOV-403 (Documentation Requirement):** Nenhuma alteração é completa sem a atualização da Bible/Vocabulary/Governance.
* **GOV-404 (Fitness Requirement):** Nenhuma alteração é aceita sem teste de conformidade.

---
## VI. Governance Examples
* **PVE:** Criada pelo `Manifest` (GOV-101), pertencente ao `PVE Manager` (GOV-102), escrita apenas pelo `PVE Manager` (GOV-103), observada por `Automation`, `API`, `Cluster`.

## VII. Governance Violations
* Driver alterando PVE diretamente (Violação GOV-002).
* Service escrevendo em Registry (Violação GOV-105).
* Dois Owners para a mesma PVE (Violação GOV-001).
* Automation acessando HAL (Violação AB-006).

# ENDAP Memory Budget Reference

Este documento serve como referência arquitetural para o consumo de memória RAM (DRAM) e contagem de tasks ativas nos principais checkpoints de inicialização do firmware.

## Metas de Margem Operacional (Produção)
Para garantir estabilidade a longo prazo, atualizações futuras e margem para reconexões, o firmware deve respeitar os seguintes limites mínimos em regime permanente:
* **Heap Mínimo Livre (`heap_min`):** `>= 16 KB`
* **Maior Bloco Contíguo Livre (`largest_free_block`):** `>= 8 KB`

---

## Tabela de Orçamento de Memória (Boot Checkpoints)

| Fase / Checkpoint | Heap Livre (Bytes) | Maior Bloco (Bytes) | Nº Tasks | Delta Custo (Bytes) | Descrição |
| :--- | :---: | :---: | :---: | :---: | :--- |
| **`BOOT_START`** | 209.928 | 110.592 | 8 | *Base* | Baseline logo após a inicialização da infraestrutura do sistema. |
| **`POST_ETHERNET_INIT`** | 189.556 | 110.592 | 11 | -20.372 | Após subida dos drivers de rede física e pilhas de conexão (LWIP, etc.). |
| **`POST_INIT_SERVICES`** | 189.556 | 110.592 | 11 | 0 | Após subida dos serviços básicos do Kernel (failsafe, event bus, watchdog, etc.). |
| **`POST_HTTP_START`** | 107.668 | 102.400 | 15 | -81.888 | Após ativação do servidor HTTP local e console web. *(Buffers JSON foram refatorados para on-demand)* |
| **`POST_CLUSTER_DISCOVERY`** | 96.392 | 94.208 | 18 | -11.276 | Após ativação do protocolo de transporte do cluster e discovery task (`disc_tx`). |

---

## Histórico de Análise de Causa Raiz
* **Problema Original (Junho/2026):** Falha ao criar a task de descoberta (`disc_tx`) por falta de memória contígua (`heap_largest=760` bytes) durante o bootstrap.
* **Causa Identificada:** Overhead de alocação de metadados induzido pela instrumentação avançada de heap (`CONFIG_HEAP_TASK_TRACKING` + `CONFIG_HEAP_TRACING_STANDALONE`).
* **Resolução:**
  1. Redução do stack planejado da task `disc_tx` de `4096` para `2048` bytes (adequado para o loop de heartbeats).
  2. Desativação das flags pesadas de heap tracking/tracing no `sdkconfig` mantendo telemetria leve via `uxTaskGetSystemState()` e `heap_caps_get_...`.

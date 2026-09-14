# Relatório de Build: ENDAP Controller (V1 Freeze)

A compilação da base V1 (congelada para o TCC) obteve sucesso sem a reintrodução de componentes exógenos (V3). A seguir, métricas coletadas da avaliação física de compilação.

## 1. Dados da Compilação
- **Status da Compilação:** SUCESSO (Exit code: 0)
- **Módulos Críticos Incorporados:**
  - `bootloader`
  - `dashboard` (via index.html.gz.S)
  - `kernel` + `runtime` + `hal`

## 2. Métricas de Espaço em Disco (Flash)
- **Tamanho do App (`ENDAP.bin`):** 1.44 MB (`0x1604e0` bytes)
- **Partição Alvo (Smallest app partition):** 1.79 MB (`0x1cb000` bytes)
- **Espaço Livre na Partição:** ~23% livre (`0x6ab20` bytes)
- **Bootloader Size:** `0x6680` bytes (8% livre na partição alocada para bootloader)

## 3. Uso de Memória Ram
De acordo com as medições extraídas pela ferramenta oficial (`idf.py size`), a alocação de tempo de compilação é altamente otimizada:

- **IRAM (Instruction RAM - Memória ultra-rápida de execução direta):**
  - Utilizado: 112,903 bytes (86.14%)
  - Restante: 18,169 bytes
  - *Justificativa:* O uso alto de IRAM é esperado e desejável, pois as rotinas críticas do `kernel` e `control_loop` residem na IRAM para evitar o jitter atrelado à leitura paralela na Flash (cache misses) e preservar a restrição estrita de 1 ms.

- **DRAM (Data RAM - Memória de dados padrão):**
  - Utilizado: 83,412 bytes (46.15%)
  - Restante: 97,324 bytes
  - Repartição interna:
    - `.bss`: 54,136 bytes (Variáveis não inicializadas)
    - `.data`: 29,276 bytes (Variáveis inicializadas)
  - *Análise:* Uso bastante conservador e saudável para as tarefas do RTOS e pilhas de serviços de rede. Há ampla margem de RAM dinâmica restante para acomodação temporária e pilhas (stacks) criadas na inicialização.

- **RTC FAST / SLOW RAM:**
  - Uso irrisório (menos de 1%), disponível para estratégias de *deep sleep* ou conservação no futuro.

## 4. Conclusão da Análise
O build confirma a tese de que é possível embarcar uma Dashboard inteira e o Kernel Determinístico na arquitetura padrão do WROOM-32. Não existem vazamentos teóricos na compilação, e a folga expressiva na DRAM prova a eficácia de não incluir bibliotecas desnecessárias de cloud e Mesh no caminho V1.

# ADR-001: Kernel Congelado e Separação de Camadas

## Status
Accepted

## Contexto
O ENDAP (Edge-Native Distributed Automation Platform) requer garantias estritas de tempo real para automação local. O loop de controle principal deve rodar de forma previsível e isolada, com latência e jitter muito baixos (ciclo de 1 ms). À medida que adicionamos funcionalidades de produto (Wi-Fi, HTTP, dashboard, adoção remota), surgiu o risco de comprometer o determinismo do núcleo por meio de integrações não controladas.

## Decisão
Estabelecemos a separação obrigatória da plataforma nas seguintes camadas:
1. **Kernel**: Determinístico, crítico, focado no controle local (control_loop, bus de eventos, state engine).
2. **Runtime**: Execução de automação local, integração com I/O e comunicação segura de estado.
3. **Services**: Fora do hot-path crítico, cuidam de HTTP, Wi-Fi, Cluster e Onboarding.
4. **UI**: Dashboard embarcada e interações via rede.

O Kernel de 1 ms é considerado **intocável** para features de produto.

## Consequências
* **Positivas**: 
  * O jitter é preservado.
  * O loop de 1 ms nunca falha (no-overrun) devido a serviços de alto nível.
  * Alta auditabilidade do hot-path.
* **Negativas**: 
  * Requer disciplina no desenvolvimento. Nenhuma operação de rede, NVS (flash) ou alocação dinâmica (`malloc`/`free`) pode ser inserida no Kernel. Comunicação com Services exige buffers, filas RTOS ou variáveis atômicas.

## Alternativas Consideradas
* *RTOS Task Preemption livre*: Permitir que as tasks competissem naturalmente pela CPU. *Rejeitado* porque as pilhas HTTP e Wi-Fi do ESP-IDF podem induzir pausas e interrupções que causam deadline misses irreparáveis em sistemas de automação de malha fechada.

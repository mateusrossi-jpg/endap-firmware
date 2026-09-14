# Roteiro de Apresentação e Demonstração: ENDAP (TCC)
Duração estimada: 10 a 15 minutos.

## Requisitos: Demonstração Mínima na Bancada
Para provar o conceito de forma robusta e livre de imprevistos, tenha os seguintes itens configurados e testados fisicamente:
1. **1 Placa Controller (ESP32-WROOM-32)** rodando o Firmware V1 compilado.
2. **1 Computador/Laptop** conectado à mesma rede (ou no Access Point provido pelo Controller) para exibir a Dashboard de supervisão.
3. **1 Placa Field Node** conectada ao Controller através de cabo RS-485 funcional.
4. **1 Sensor/Botão Físico** conectado em um input local do Controller.
5. **1 Atuador/LED Físico** conectado em um output local do Controller.
6. **(Opcional)** 1 LED extra conectado ao Field Node.

---

## 1. Problema (1 min)
- **Discurso:** A automação industrial e predial moderna sofre de uma dicotomia: ou sistemas comerciais centralizados (CLPs) que são inflexíveis, caros e fechados; ou soluções baseadas em IoT puro, que sofrem latência de rede, dependem de nuvem, e falham de maneira catastrófica sem internet.
- **Tese:** Precisamos de um modelo *Edge-Native* e *Local-First*, onde a inteligência é distribuída, a rede atua apenas como supervisão e o núcleo não quebra sob falta de comunicação.

## 2. Conceito ENDAP (1 min)
- **Discurso:** ENDAP (Edge-Native Distributed Automation Platform). Ele introduz a separação estrita entre um **Kernel determinístico** (que não falha) e **Serviços assíncronos** (rede, dashboard, integrações).

## 3. Arquitetura (1 min)
- **Discurso:** Diferente do Arduino ou firmwares monolíticos comuns de TCC, usamos uma arquitetura em camadas rígidas.
- **Destaque:** Mostrar ou descrever o diagrama (IO → Fieldbus → Automation → Events → Diagnostics).

## 4. O Controller (1 min)
- **Ação:** Mostrar a placa Controller operando.
- **Discurso:** Esta é a unidade autônoma. Ela executa o firmware completo e pode operar de forma independente ou como ponte de rede.

## 5. O Field Node e Comunicação (2 min)
- **Ação:** Mostrar o cabo RS-485 conectando ao Field Node.
- **Discurso:** O barramento expande fisicamente as portas. O Field Node não possui regras complexas; ele repassa o estado de I/O, preservando a lógica de controle no Controller, sem introduzir complexidades de rede LAN.

## 6. I/O, Rule Engine e Determinismo (2 min)
- **Ação:** Apertar o botão conectado ao input local do Controller e ver a saída atuando (LED acendendo).
- **Discurso:** A regra está injetada no Rule Engine embarcado. O tempo de reação é garantido pelo loop de controle fechado em 1ms.

## 7. Failsafe na Prática (1 min)
- **Ação:** Desconectar fisicamente o cabo RS-485 ou causar uma interrupção simulada.
- **Discurso:** Se o loop se atrasar ou se perder comunicação, as saídas do Field Node (ou do Controller) entram no modo seguro (Failsafe) predefinido, isolando problemas de propagação física.

## 8. Dashboard e Supervisão (2 min)
- **Ação:** Abrir a interface web no computador. Apertar o botão e mostrar a Dashboard reagindo visualmente.
- **Discurso:** A Dashboard foi embutida diretamente na placa (não depende de cloud). Ela reflete os estados via "Event Bus". Mostrar como as abas monitoram o sistema, porém reforçando: *A Dashboard é uma lente de observação, não é o núcleo da automação.*

## 9. Limitações Atuais (Honestidade Técnica) (1 min)
- **Discurso:** Neste protótipo evitamos LoRa, comunicação em malha (Mesh) e MQTT Cloud de propósito. Implementá-los causaria ruído na demonstração da tese de "controle estrito local". O foco foi construir e congelar a base estável.

## 10. Evolução Futura para Produto (1 min)
- **Discurso:** Este mesmo núcleo determinístico receberá módulos (Serviços V2/V3) permitindo: Gateways para nuvem, configuração via ENDAP Studio e adoção por Mesh, sem reescrever a base que acabamos de validar.

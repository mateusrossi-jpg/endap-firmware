# ADR-004: Network v1 (Failover + Hysteresis)

## Status
Accepted

## Contexto
A resiliência operacional é o coração do ENDAP. Em plantas industriais, redes Wi-Fi podem cair por ruído, mas a comunicação local (ex: via RS485) pode continuar ativa. Se um Gateway não puder enxergar o nó pela rede principal, precisa haver tolerância imediata a falhas.

## Decisão
Implementamos a máquina de estados "Network v1" gerida de forma segregada. Ela suporta definição de *Primary Transport* (ex: Wi-Fi) e *Fallback Transport* (ex: RS485).
Se o primário falha, o estado da rede transita para `DEGRADED`, mas sem interromper o serviço, mudando instantaneamente o envio do *cluster payload* para o fallback.
Foi introduzida a **Histerese de Recuperação** (Hysteresis delay, tipicamente 15s) para evitar que instabilidades transitórias do link primário fiquem disparando flutuações rápidas (flapping) entre o transporte primário e secundário.

## Consequências
* **Positivas**:
  * Maior resiliência de cluster. Um Gateway não perde contato com um Field Node em ambientes com Wi-Fi ruidoso desde que exista a camada serial/RS485 disponível.
  * Métrica observável e clara de estado: a UI reflete exatamente a degradação e falhas ocorridas (`failover_count`).
* **Negativas**:
  * Complexidade de roteamento e envio dual. Cada mensagem enviada à camada de rede exige verificar a interface preferencial do instante.

## Alternativas Consideradas
* *Failover Imediato bidirecional*: Permitir uso de ambos os transportes ao mesmo tempo com roteamento por métrica, no padrão IP/OSPF. *Rejeitado*. O overhead em microcontroladores seria irreal na v1. Fallback rígido com histerese é mais simples, consome zero CPU no Kernel, e traz 90% da robustez desejada.

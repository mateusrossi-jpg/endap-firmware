# ADR-002: NodeProfile com Templates Estáticos

## Status
Accepted

## Contexto
O sistema distribuído do ENDAP exige a presença de múltiplos perfis de nós (ex: Gateway, Field Node, Relay, Sensor). Na versão 1 do produto, o processo de adoção e configuração precisa ser simples, de forma que um novo hardware possa assumir instantaneamente um papel e inicializar corretamente as pinagens e periféricos necessários (I/O, SPI, Ethernet).

## Decisão
Implementamos a abstração `NodeProfile` baseada em templates estáticos (armazenados em Flash via `const struct`). 
Os perfis suportados (`NODE_PROFILE_GATEWAY`, `NODE_PROFILE_FIELD`, etc.) são definidos por meio de um simples `enum` salvo em NVS.

## Consequências
* **Positivas**:
  * Carregamento em tempo de boot (inicialização) é instantâneo e não exige parse JSON.
  * Baixíssimo consumo de RAM; todos os mapas de I/O, descrições e capacidades residem na Flash do ESP32 (`.rodata`).
  * Código altamente coeso: apenas o `enum` do perfil atual é salvo em NVS. O resto é expandido automaticamente no boot.
* **Negativas**:
  * Na versão 1, modificar canais e hardware base exige recompilar o firmware se for algo totalmente customizado (fora da expansão MCP/ADC padrão). 

## Alternativas Consideradas
* *Mapeamento totalmente dinâmico via JSON na NVS*: *Rejeitado*. O JSON adicionaria complexidade prematura na v1, aumento de tempo de parse no boot, risco de configuração corrompida por falha de escrita, e consumo extra de heap antes que o core system suba.

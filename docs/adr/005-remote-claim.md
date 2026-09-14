# ADR-005: Claim Remoto via Frame Binário Fixo

## Status
Accepted

## Contexto
Na infraestrutura distribuída do ENDAP, o processo de "Adoção" (Claim) é iniciado no nó Gateway. O Gateway (que hospeda a Dashboard v1) comanda um nó em estado PENDING (via broadcast UDP ou serial) a se submeter e configurar o Perfil e o ID indicados. Para garantir previsibilidade e portabilidade entre todos os transportes (UDP, TCP, RS485), o payload de controle precisa ser uniforme e robusto.

## Decisão
Para o comando de Remote Claim, implementamos um Frame Binário Estático `endap_remote_claim_msg_t` com um magic word (`CLM1` = `0x434C4D31`). 
Esta mensagem é enviada nativamente pelo layer abstrato `cluster_transport`. O Gateway impõe um timeout estrito (3 a 5 segundos) aguardando o reconhecimento síncrono da adoção pelo nó remoto (via semáforos/flags FreeRTOS), antes de reportar sucesso no front-end e atualizar seu próprio registro local (`node_registry`).

**Regra Operacional Crítica:** Para onboarding de um Field Node, a transição do Node Registry do Gateway para ACTIVE depende de um ENDAP_REMOTE_CLAIM_RESP válido, correspondente ao node_id solicitado e com status de sucesso. O gateway jamais pode elevar o estado do nó localmente sem a prova de submissão do nó pela rede.

## Consequências
* **Positivas**:
  * Processamento do pacote em $O(1)$ RAM e processamento; o nó remoto pode inspecionar e verificar se é um "claim frame" com zero parse string/JSON e alocação dinâmica.
  * Portável imediatamente entre Ethernet, Wi-Fi e porta Serial, respeitando os tamanhos limitados do MTU de serial.
  * Consistência transacional distribuída mantida sob controle local do Gateway.
* **Negativas**:
  * Formato binário packed é estrito e requer recompilação/mudança global de magic words (ex: `CLM2`) se a struct crescer no futuro.

## Alternativas Consideradas
* *Requisição HTTP RestFul `POST /api/onboarding/claim` enviada do Gateway para o Field Node*: *Rejeitado*. Depender de stack HTTP (L7) em cima de rede Wi-Fi que talvez nem esteja com IP fixado ou acessível por DHCP criaria instabilidade. O uso de broadcast multicast/UDP L2/L4 ou Serial Direto (L2) na inicialização assegura que nós sem infraestrutura de IP definida sejam capturados no ato.

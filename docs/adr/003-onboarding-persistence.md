# ADR-003: Onboarding e Persistência NVS

## Status
Accepted

## Contexto
Um nó precisa saber com segurança se já pertence a um cluster ou se é hardware virgem de fábrica. Caso perca energia, deve retornar exatamente para o mesmo estado. Adicionalmente, era necessário um mecanismo de Factory Reset para desvincular um nó de um Gateway e reiniciá-lo de forma segura.

## Decisão
O estado de Onboarding é salvo como um blob binário de versão fixa (`ob_cfg_v1` da struct `endap_onboarding_config_t`) em uma namespace segregada na NVS (`onboarding`). 
Estados bem definidos comandam a lógica: `PENDING` -> `DISCOVERED` -> `CLAIMED` -> `ACTIVE`.
Para o **Factory Reset**, toda a namespace da NVS de onboarding, de profile (`dev_profile`) e do registro de nós é apagada de uma vez. Em seguida, o `NODE_PROFILE_FIELD` (fallback seguro) é aplicado à memória.

## Consequências
* **Positivas**:
  * Alta resiliência a corrupção (se a versão/magicsource mudar, a configuração atual cai para PENDING via safe fallback).
  * Consistência garantida: a transição de estado está vinculada atomicamente na NVS.
  * O Factory Reset varre todos os rastros de adoções anteriores, deixando o nó plenamente reutilizável.
* **Negativas**:
  * Alterações no tamanho ou layout das structs do blob exigem migração cuidadosa (bump de versão) nas futuras V2, V3.

## Alternativas Consideradas
* *Salvar cada propriedade (nome, perfil, estado, id) como uma chave String/Int na NVS.*: *Rejeitado*. Causaria latência no boot com dezenas de acessos à NVS e elevaria a probabilidade de falhas parciais (ex: salva o estado `ACTIVE` mas o ESP reinicia antes de salvar o `gateway_id`). O salvamento de struct em blob resolve a atomicidade mínima sem uso pesado de transações.

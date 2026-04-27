# ENDAP Demo Validation

## Objetivo

Checklist prático para validar o ENDAP v1 em bancada antes da apresentação do TCC.

Fluxos cobertos:

- Wi-Fi AP
- Wi-Fi STA
- Ethernet / RJ45
- RS485 bench
- Cluster básico

## Pré-condições

- `idf.py build` executa sem erro
- placa alvo ESP32-WROOM-32 disponível
- firmware gravado com `idf.py -p <porta> flash`
- monitor serial aberto com `idf.py -p <porta> monitor`

## 1. Boot básico

Esperado no log:

- `Boot ENDAP`
- `Init Infrastructure`
- `Init Domain`
- `Init Services`
- `Control loop started (1ms)`

Esperado em operação:

- sem reset em loop
- sem `deadline fault`
- dashboard HTTP disponível quando houver link IP

## 2. Wi-Fi AP

Quando usar:

- nó sem credenciais STA gravadas

Passos:

1. ligar a placa sem credenciais Wi-Fi
2. confirmar SSID `ENDAP_SETUP-<node_id>`
3. conectar um notebook/celular ao AP
4. abrir `http://192.168.4.1/`

Esperado:

- dashboard abre sem redirecionamento quebrado
- modo mostra operação standalone
- rota ativa mostra `wifi-ap 192.168.4.1`
- SSID do AP expõe o node local para formação do cluster offline
- fieldbus aparece como `RS485` ou `RS485 SELF-TEST`

## 3. Wi-Fi STA

Passos:

1. no dashboard, salvar SSID e senha válidos
2. aguardar reconexão
3. identificar IP entregue pelo roteador ou pelo log
4. abrir dashboard pelo IP STA

Esperado:

- log mostra `WiFi conectado com IP`
- dashboard continua acessível
- rota ativa mostra `wifi-sta <ip>`
- cluster pode iniciar por `wifi-udp`

## 4. Ethernet / RJ45

Pré-condição:

- perfil com backend Ethernet habilitado
- pinagem W5500 ou hardware Ethernet configurado

Passos:

1. conectar o cabo RJ45
2. energizar a placa
3. acompanhar logs de Ethernet
4. abrir dashboard pelo IP entregue na rede

Esperado:

- log mostra `Link Ethernet conectado`
- depois `Ethernet com IP pronto`
- dashboard mostra rota ativa `ethernet <ip>`
- cluster pode migrar para `ethernet-udp`

## 5. RS485 bench

Passos:

1. subir o firmware principal
2. validar estado do barramento no dashboard
3. observar métricas de retries, timeout, CRC e latência

Esperado:

- sem crescimento contínuo de `timeouts`
- sem crescimento anormal de `crc`
- `avg` e `max latency` coerentes
- diagnóstico do barramento referencia `RS485`, não Wi-Fi/Ethernet

## 6. RS485 self-test

Quando usar:

- validação em bancada com uma única placa

Passos:

1. manter self-test habilitado
2. abrir dashboard
3. observar latência e alertas do barramento

Esperado:

- alerta `Fieldbus self-test active`
- rótulo `RS485 SELF-TEST`
- métricas de latência atualizando sem erro crítico

## 7. Cluster básico

Quando usar:

- duas placas com link IP funcional

Passos:

1. colocar ambos os nós na mesma rede Wi-Fi STA ou Ethernet
2. subir gateway e nó de campo
3. observar descoberta e contagem de nós
4. validar ownership de IO no dashboard

Alternativa sem roteador:

1. subir duas placas sem credenciais salvas
2. aguardar alguns segundos para a eleição automática do host Wi-Fi
3. confirmar que a placa de menor `node_id` manteve o AP `ENDAP_SETUP-<node_id>`
4. confirmar que a placa de maior `node_id` migrou para `wifi-sta`
5. abrir a dashboard no host AP e validar a descoberta do peer

Esperado:

- `Cluster Manager iniciado`
- `Cluster transport ativo`
- `Cluster discovery iniciado`
- dashboard mostra `c_online >= 2`
- `c_master` consistente

## 8. Sinais de reprovação

Interromper a apresentação técnica se aparecer:

- `deadline fault`
- reboot espontâneo
- dashboard inacessível no link esperado
- rota ativa incorreta
- barramento com timeout contínuo
- cluster oscilando entre online/offline sem causa física

## 9. Ordem recomendada para demo

1. boot e dashboard local
2. Wi-Fi AP
3. Wi-Fi STA
4. RS485 e automação
5. Ethernet / RJ45
6. cluster básico

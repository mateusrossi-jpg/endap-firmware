# ENDAP Transport Runtime Validation

Checklist curto para validar a rodada de gating por transporte no hardware real.

## Objetivo

Confirmar que transporte desabilitado passa a ser tratado como inexistente em:

- boot/runtime
- health e alertas
- resumo tecnico
- blocos de conexao
- dashboard

## Preparacao

1. gravar o firmware atualizado
2. abrir monitor serial
3. acessar a dashboard pelo enlace atual do no
4. usar `bash tools/set_transport_profile.sh <base_url> <modo> --reboot` para alternar cenarios

Exemplo:

```bash
bash tools/set_transport_profile.sh http://192.168.4.1 wifi+ethernet --reboot
```

## Matriz de cenarios

### 1. `wifi`

Esperado:

- boot inicia apenas Wi-Fi
- Ethernet nao aparece na dashboard
- RS485 nao aparece na dashboard
- sem `Barramento com falhas`
- `/api/status` retorna `wifi_enabled=1`, `ethernet_enabled=0`, `rs485_enabled=0`

### 2. `ethernet`

Esperado:

- boot inicia apenas Ethernet
- Wi-Fi nao aparece na dashboard
- RS485 nao aparece na dashboard
- sem endpoints de recovery Wi-Fi na UI
- `/api/status` retorna `wifi_enabled=0`, `ethernet_enabled=1`, `rs485_enabled=0`

### 3. `rs485`

Esperado:

- boot inicia apenas RS485
- Wi-Fi nao aparece na dashboard
- Ethernet nao aparece na dashboard
- nada de timeout/retry se o barramento estiver desabilitado em outro perfil
- `/api/status` retorna `wifi_enabled=0`, `ethernet_enabled=0`, `rs485_enabled=1`

### 4. `wifi+ethernet`

Esperado:

- Wi-Fi e Ethernet aparecem
- RS485 nao aparece
- sem alerta de barramento
- `/api/status` retorna `wifi_enabled=1`, `ethernet_enabled=1`, `rs485_enabled=0`

### 5. `ethernet+rs485`

Esperado:

- Ethernet e RS485 aparecem
- Wi-Fi nao aparece
- recovery Wi-Fi nao aparece
- `/api/status` retorna `wifi_enabled=0`, `ethernet_enabled=1`, `rs485_enabled=1`

### 6. `wifi+rs485`

Esperado:

- Wi-Fi e RS485 aparecem
- Ethernet nao aparece
- nada de bloco cabeado na dashboard
- `/api/status` retorna `wifi_enabled=1`, `ethernet_enabled=0`, `rs485_enabled=1`

## Criterios de aprovacao

- transporte desabilitado nao inicializa driver nem task
- transporte desabilitado nao gera timeout/retry/alerta falso
- transporte desabilitado nao aparece em `panel`, `connect`, `nodes`
- `/api/nodes` nao expoe `last_transport` de enlace local desabilitado
- a dashboard continua coerente apos reboot e reconexao

## Sinais de reprovação

- `WIFI_DISABLED` nao respeitado nos endpoints de recovery
- `Barramento com falhas` aparecendo com RS485 desabilitado
- `wifi-udp`, `ethernet-udp` ou `rs485-cluster` aparecendo na lista de nos quando o enlace local correspondente estiver desligado
- bloco de Wi-Fi/Ethernet/RS485 visivel fora da combinacao configurada

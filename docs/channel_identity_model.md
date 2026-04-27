# ENDAP Channel Identity Model

Este documento congela a separacao entre identidade fisica/local do no e identidade logica da instalacao.

## Decisao Oficial

- A base do firmware trabalha com canais locais por no: `IN1`, `IN2`, `OUT1`, `OUT2`.
- O firmware expoe backend fisico, endereco local e estado do canal.
- O gateway/API/dashboard criam a visao global da instalacao: `IN1..IN8`, `OUT101..OUT104`, aliases, grupos e ordenacao entre ESP1, ESP2 e ESP3.
- Regras de automacao devem apontar para canal logico, nunca para GPIO direto.

## Modelo Local Do No

Todo no deve conseguir expor uma lista de canais locais com estes campos:

```c
typedef enum {
    ENDAP_CHANNEL_INPUT = 0,
    ENDAP_CHANNEL_OUTPUT = 1,
    ENDAP_CHANNEL_ANALOG_INPUT = 2,
} node_channel_direction_t;

typedef enum {
    ENDAP_CHANNEL_BACKEND_GPIO = 0,
    ENDAP_CHANNEL_BACKEND_MCP23X17 = 1,
    ENDAP_CHANNEL_BACKEND_ADC_NATIVE = 2,
    ENDAP_CHANNEL_BACKEND_ADS1115 = 3,
} node_channel_backend_t;

typedef struct {
    uint16_t channel_id;          /* Id interno/legado do runtime atual. */
    uint16_t local_index;         /* 1, 2, 3... dentro do proprio no. */
    char local_code[12];          /* IN1, IN2, OUT1, OUT2... */
    node_channel_direction_t direction;
    node_channel_backend_t backend;
    uint8_t backend_instance;     /* MCP0, ADS0... */
    uint8_t endpoint_index;       /* Pino/canal dentro do backend. */
    int16_t gpio;                 /* GPIO quando backend == GPIO. */
    char backend_address[24];     /* GPIO2, MCP0:A0, ADS0:CH2... */
    char display_name_local[32];  /* Nome simples local. */
    uint8_t active_low;
    int32_t value;
} node_channel_t;
```

No codigo atual, estes campos ja aparecem de forma aditiva em:

- `GET /api/profile`
- `GET /api/public/profile`
- `GET /api/status`

## Mapa Logico Da Instalacao

O gateway mantem o mapa de apresentacao da instalacao:

```c
typedef struct {
    uint32_t node_id;
    uint8_t kind;                 /* 0=input, 1=output. */
    uint16_t channel_id;
    char local_code[12];          /* OUT1 do no, por exemplo. */
    char global_code[24];         /* OUT105, IN9... */
    char alias[40];               /* Relay Sala, Sensor Porta... */
    char room[20];                /* Sala, quadro, setor... */
    char group[20];               /* Iluminacao, bombas... */
    int16_t sort_order;           /* Ordenacao opcional da instalacao. */
    char visibility[12];          /* visible, hidden, tech... */
    char notes[48];               /* Observacao curta de campo. */
} installation_channel_map_t;
```

No codigo atual, o mapa e persistido e exposto por:

- `GET /api/installation/channel-map`
- `POST /api/installation/channel-map/save`

O endpoint aceita `global_code` e `global_id` como sinonimos. A resposta mantem os dois
campos para compatibilidade, mas a identidade canonica continua sendo `global_code`.

## Faixas Globais Padrao

A numeracao global e derivada no gateway/dashboard:

- ESP1: entradas `IN1..IN8`, saidas `OUT101..OUT104`
- ESP2: entradas `IN9..IN16`, saidas `OUT105..OUT108`
- ESP3: entradas `IN17..IN24`, saidas `OUT109..OUT112`

Isso e uma visao de instalacao, nao a identidade fisica do firmware.

## Regra Para Dashboard E API Publica

- A tela de hardware local deve mostrar `ESP2.OUT1 -> GPIO2` ou `ESP2.IN1 -> MCP0:A0`.
- A tela de instalacao deve mostrar `OUT105 = ESP2.OUT1` e opcionalmente um alias.
- O usuario comum deve operar por `ESP2.OUT1`, `OUT105` ou alias.
- GPIO, MCP e ADS sao detalhes tecnicos do canal local.

## Regra Para Automacao

As regras devem evoluir para referenciar canal logico:

- preferido: `node_id + local_code`
- tambem aceitavel: `global_code`
- amigavel para UX: `alias`

O runtime resolve isso para backend/endereco fisico somente no ponto de aplicacao do comando.

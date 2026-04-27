# RS485 Slave Test

Firmware minimo para uma segunda ESP32 responder `ACK` ao master ENDAP no mesmo formato de frame RS485.

## Build

```bash
idf.py -C rs485_slave_test build
```

## Flash

```bash
idf.py -C rs485_slave_test -p /dev/ttyUSBX flash monitor
```

## Pinagem Padrao

- `TX`: GPIO `25`
- `RX`: GPIO `26`
- `DE/~RE`: GPIO `27`
- `UART`: `UART2`
- `Baud`: `115200`
- `Node ID`: `1`

## Teste

1. Grave o firmware principal `ENDAP` em uma ESP32.
2. Grave `rs485_slave_test` em outra ESP32.
3. Ligue `A/B/GND` entre os transceptores RS485 das duas placas.
4. Abra o monitor serial nas duas.
5. O escravo deve logar `ACK node=1 ...`.
6. O master deve parar de acumular timeout/retry para o nó `1`.

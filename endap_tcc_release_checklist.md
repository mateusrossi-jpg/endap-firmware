# Checklist de Release para o TCC (ENDAP V1)

Esta é a lista final de verificação técnica. Use-a para confirmar o estado do projeto ao iniciar a apresentação.

- [x] **Build reproduzível:** `idf.py build` executa e passa sem bloqueios fatais.
- [x] **Firmware gera ENDAP.bin:** Arquivo gerado com êxito, partição suportada.
- [x] **Dashboard incorporada:** Recurso web `index.html.gz.S` está injetado na Flash com sucesso.
- [x] **Controller inicializa:** `endap_boot_task` confirmada operando em `main.c`.
- [ ] **I/O inicializa:** Testar via bancada; Driver e HAL estão no build.
- [ ] **Rule Engine inicializa:** O componente Automation Engine faz parte do target do Cmake.
- [ ] **Failsafe inicializa:** Incorporado na lógica de snapshot/I/O.
- [ ] **Event Bus funcionando:** Validar pela interface web e debug outputs que os eventos são distribuídos.
- [ ] **RS-485 funcionando conforme estado atual:** Requer verificação física de troca de pacotes; `rs485_engine` e hal estão compilados.
- [ ] **Field Node detectável conforme implementação atual:** Checar estado visualizado pelo Fieldbus/Supervisão local.
- [ ] **APIs funcionando:** Confirmar que conexões POST/GET no IP do Controller retornam sucesso.
- [ ] **Dashboard funcional:** Abas, informações e comunicação WebSocket/HTTP provando o Event Bus.
- [x] **Nenhuma dependência crítica de Internet:** Aplicação encapsulada via AP interno/DHCP próprio (Captive e Ethernet/Wi-Fi locais ativados).
- [x] **Nenhuma feature V3 reintroduzida:** Diretiva `set(EXCLUDE_COMPONENTS v3)` confirmada e protegida.
- [x] **Nenhuma alteração desnecessária no Core:** Loop crítico foi preservado isoladamente.
- [x] **Documentação da arquitetura:** (`endap_tcc_current_state.md` e `endap_tcc_gap_analysis.md`) gerada.
- [x] **Roteiro de apresentação:** (`endap_tcc_demo.md`) gerado.
- [x] **Limitações conhecidas documentadas:** Refletidas no roteiro (exclusão de LoRa, Mesh, etc).

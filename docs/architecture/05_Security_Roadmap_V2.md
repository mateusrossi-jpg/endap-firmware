# Arquitetura de Segurança V2 (Zero Trust Edge)

## 1. Contexto e Motivação (TCC / V1)
Na versão atual (V1) do firmware **ENDAP**, a segurança e o controle de acesso baseiam-se em **Capabilities Baseadas em Token/Sessão** (ex: `AUTH_CAP_SECURITY_ADMIN`, `AUTH_CAP_DASHBOARD_READ`). O painel web e os nós se comunicam ativamente sobre as APIs HTTP para adoção, configuração e telemetria. 

Embora suficiente para a validação da prova de conceito (PoC) e cenários controlados, aplicações **SCADA e Industriais** demandam topologias onde os dispositivos na ponta (*Edge*) operem invisíveis à rede corporativa, sem expor APIs diretamente. O objetivo deste documento é formalizar o roadmap de segurança corporativa para o **ENDAP V2**.

---

## 2. Visão "Zero Trust" Local e Edge Gateway

A principal premissa para a V2 é a remoção da exposição direta das APIs dos nós operacionais (*Field Nodes*).

### 2.1. Topologia em Estrela Isolada
1. **Edge Gateway Central:** Um nó de gateway robusto (ex: ESP32-S3 ou um IPC Linux) atua como a única fronteira de rede exposta.
2. **Backbone Isolado:** Os *Field Nodes* comunicam-se com o Gateway através de um barramento dedicado, fisicamente isolado (como **RS485 OSDP/Modbus** criptografado) ou uma VLAN restrita (Wi-Fi 802.1x fechado).
3. **Invisibilidade Operacional:** Nenhuma API HTTP dos *Field Nodes* estará disponível para a LAN corporativa. O acesso ao Dashboard ocorrerá **apenas através do Edge Gateway**, que roteia e aplica proxy reverso seguro aos dados de telemetria.

---

## 3. mTLS (Mutual TLS) Nativo

Para garantir a criptografia e a identidade ponto a ponto (Node-to-Node e Node-to-Gateway) sem depender de sessões HTTP suscetíveis a roubo (Hijacking), o firmware implementará mTLS nativo:

* **PKI Interna Integrada (Onboarding):** Durante a adoção de um nó, o Gateway gera um certificado X.509 cliente para a nova placa.
* **Socket Seguro Obrigatório:** O servidor HTTP interno da placa (`httpd_start`) exigirá o certificado do Gateway. Apenas componentes internos com a chave criptográfica correta conseguirão realizar requisições REST/WebSocket na placa.
* **Erradicação de Senhas Fixas:** O uso de credenciais baseadas em texto ou cookies é abolido do tráfego interno (Machine-to-Machine).

---

## 4. Evolução para WebSockets Assinados (HMAC)

Atualmente, o *polling* contínuo de `/api/pve` (Process Variables) expõe metadados de requisição excessivos e sobrecarrega a CPU com a quebra e criação de sockets TCP. 

Na arquitetura V2:
* As APIs REST param de gerenciar as leituras em tempo real.
* Um único túnel **WebSocket** é estabelecido entre o Field Node e o Gateway no momento de inicialização.
* Toda a carga (payload) trafegada será assinada via **HMAC-SHA256**, validada pelo Kernel do *Control Loop*, ignorando qualquer injeção espúria e protegendo contra ataques de *Replay* industrial.

---

## 5. Endurecimento do Kernel (Hardening) e Desativação Segura

Funcionalidades de diagnóstico crítico implementadas para a prova de conceito e demonstração — como as rotas de *Chaos Monkey* (`/api/chaos/inject_delay` e `/api/chaos/crash`) — não devem fazer parte de um build de produção *release*.

* **Flags de Compilação (`sdkconfig`):** Variáveis de estresse intencional (`g_chaos_inject_delay`) serão condicionalmente compiladas através de uma macro `CONFIG_ENDAP_ENABLE_CHAOS_TESTING`.
* Em ambientes industriais, este recurso de injeção estará ausente do binário final, blindando o software de exploração via escalonamento de privilégios.

---

## Conclusão

A transição da **V1 (Base com Capabilities)** para a **V2 (Zero Trust com mTLS e Gateways Isolados)** eleva a maturidade do projeto ENDAP do âmbito acadêmico/protótipo à aderência dos rigorosos padrões industriais IEC 62443 (Segurança em Automação Industrial e Sistemas de Controle).

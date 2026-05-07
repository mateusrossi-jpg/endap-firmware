ENDAP Security V2 P1 — proteção de sessão, permissões e Fail-Safe
===================================================================

Objetivo
--------
Este pacote fecha a próxima etapa após o Fail-Safe V1/V1.1: proteger ações críticas, separar a permissão de escrita de fail-safe e tornar o fluxo de sessão mais confiável para dashboard/API.

Arquivos alterados
------------------
- components/security/auth/include/auth.h
- components/security/auth/auth.c
- components/runtime/http_server/http_server.c
- components/dashboard/index.html

Mudanças principais
-------------------
1. Nova capacidade de segurança:
   - AUTH_CAP_FAILSAFE_WRITE
   - Admin possui essa capacidade.
   - Integrator/Installer possui essa capacidade.
   - Operator não possui.
   - Viewer não possui.

2. Endpoints críticos de fail-safe agora exigem AUTH_CAP_FAILSAFE_WRITE:
   - POST /api/failsafe/save
   - POST /api/failsafe/output/save
   - POST /api/failsafe/rearm
   - POST /api/failsafe/output/reset
   - POST /api/failsafe/output/test

3. Leitura do fail-safe continua protegida por leitura da dashboard:
   - GET /api/failsafe
   - GET /api/failsafe/outputs

4. Bootstrap local ficou mais seguro:
   - Antes do primeiro admin, o bootstrap continua permitindo leitura mínima da dashboard e criação inicial do admin.
   - Escritas críticas não passam abertas no bootstrap.
   - Tentativa de escrita crítica sem autenticação no bootstrap retorna 403 com erro bootstrap_read_only.

5. Respostas de autenticação/permissão foram padronizadas:
   - 401 Unauthorized quando não autenticado.
   - 403 Forbidden quando autenticado sem permissão ou em bootstrap read-only.
   - JSON: {"ok":false,"error":"..."}

6. Sessão/API:
   - Mantido cookie HttpOnly já existente.
   - Mantido header X-ENDAP-Session já existente.
   - Adicionado suporte opcional a Authorization: Bearer <token> para testes via curl/integrações.
   - Adicionado alias GET /api/auth/me apontando para o status de sessão já existente.

7. Dashboard:
   - fail-safe agora usa a permissão failsafe_write, não profile_write.
   - Viewer visualiza, mas não altera.
   - Operator opera I/O manual, mas não altera fail-safe.
   - Integrator/Admin podem salvar, rearmar e testar fail-safe.
   - Bootstrap agora fica visualmente mais conservador: leitura liberada, ações críticas bloqueadas.

8. Correção adicional:
   - Corrigido o snprintf de auditoria do failsafe_save_handler, removendo argumento duplicado/desalinhado.

Validação feita neste ambiente
------------------------------
- Dashboard JavaScript extraído e validado com:
  node --check dashboard_script.js

Não foi possível validar aqui
-----------------------------
- idf.py build
Motivo: este ambiente não possui ESP-IDF/idf.py instalado.
O erro retornado foi:
  bash: line 1: idf.py: command not found

Teste recomendado após aplicar
------------------------------
1. Compilar:
   idf.py build

2. Fazer flash e testar login normalmente pela dashboard.

3. Validar sessão:
   curl -i -b cookies.txt http://IP_DO_ENDAP/api/auth/me

4. Validar endpoint protegido sem sessão:
   curl -i -X POST http://IP_DO_ENDAP/api/failsafe/output/save \
     -H 'Content-Type: application/x-www-form-urlencoded' \
     --data 'output_id=1&enabled=1'

   Esperado:
   - 401 Unauthorized se o sistema já estiver configurado e sem sessão.
   - 403 Forbidden / bootstrap_read_only se ainda estiver em bootstrap aberto.

5. Login via curl, se quiser testar API:
   curl -i -c cookies.txt -X POST http://IP_DO_ENDAP/api/auth/login \
     -H 'Content-Type: application/x-www-form-urlencoded' \
     --data 'username=admin&password=SUA_SENHA'

6. Validar sessão autenticada:
   curl -i -b cookies.txt http://IP_DO_ENDAP/api/auth/me

7. Validar escrita de fail-safe autenticada como Admin/Integrator:
   curl -i -b cookies.txt -X POST http://IP_DO_ENDAP/api/failsafe/output/save \
     -H 'Content-Type: application/x-www-form-urlencoded' \
     --data 'output_id=1&enabled=1&boot_action=force_off&comm_loss_action=force_off&runtime_fault_action=force_off&safe_value=0&recovery_mode=manual'

8. Testes funcionais obrigatórios:
   - F5/reload mantém sessão válida.
   - Logout volta para login.
   - Troca de senha continua funcionando.
   - Viewer não salva/rearma/testa fail-safe.
   - Operator não salva/rearma/testa fail-safe.
   - Integrator salva/rearma/testa fail-safe.
   - Admin salva/rearma/testa fail-safe.
   - Fail-safe continua persistindo em NVS após reboot.

Observação importante
---------------------
Este patch não remove toda possibilidade física de erro de instalação, comando humano indevido ou falha elétrica externa. Ele fecha a proteção lógica/API/dashboard para a etapa Security V2 P1. Para risco industrial completo, ainda faltam testes em hardware, matriz de regressão, validação elétrica, fail-safe físico e revisão de release.

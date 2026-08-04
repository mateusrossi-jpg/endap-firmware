#!/bin/bash

# ENDAP v1 E2E Test Helpers
# Usage: ./test_api.sh <command> <IP>

CMD=$1
IP=$2

if [ -z "$CMD" ] || [ -z "$IP" ]; then
    echo "Uso: $0 <comando> <IP_DO_NO>"
    echo "Comandos disponíveis:"
    echo "  metrics       - Busca as métricas de rede"
    echo "  nodes         - Lista os nós conhecidos (Gateway)"
    echo "  status        - Busca o status atual do onboarding/perfil"
    echo "  reset         - Aciona o Factory Reset no nó"
    exit 1
fi

echo "--- Executando comando '$CMD' em http://$IP ---"

case $CMD in
    metrics)
        curl -s -X GET "http://$IP/api/network/metrics" | jq . || curl -s -X GET "http://$IP/api/network/metrics"
        ;;
    nodes)
        curl -s -X GET "http://$IP/api/nodes" | jq . || curl -s -X GET "http://$IP/api/nodes"
        ;;
    status)
        curl -s -X GET "http://$IP/api/onboarding/status" | jq . || curl -s -X GET "http://$IP/api/onboarding/status"
        ;;
    reset)
        echo "Atenção: Disparando Factory Reset..."
        curl -s -X POST "http://$IP/api/system/factory-reset" | jq . || curl -s -X POST "http://$IP/api/system/factory-reset"
        ;;
    *)
        echo "Comando desconhecido."
        ;;
esac
echo -e "\n--- Concluído ---"

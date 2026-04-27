#!/usr/bin/env bash

set -euo pipefail

if [[ $# -lt 2 || $# -gt 3 ]]; then
    cat <<'EOF'
Uso:
  bash tools/set_transport_profile.sh <base_url> <mode> [--reboot]

Modos:
  wifi
  ethernet
  rs485
  wifi+ethernet
  ethernet+rs485
  wifi+rs485

Exemplos:
  bash tools/set_transport_profile.sh http://192.168.4.1 wifi
  bash tools/set_transport_profile.sh http://192.168.4.1 ethernet+rs485 --reboot
EOF
    exit 1
fi

base_url="${1%/}"
mode="$2"
reboot_flag="${3:-}"

wifi_enabled=0
ethernet_enabled=0
rs485_enabled=0

case "$mode" in
    wifi)
        wifi_enabled=1
        ;;
    ethernet)
        ethernet_enabled=1
        ;;
    rs485)
        rs485_enabled=1
        ;;
    wifi+ethernet|ethernet+wifi)
        wifi_enabled=1
        ethernet_enabled=1
        ;;
    ethernet+rs485|rs485+ethernet)
        ethernet_enabled=1
        rs485_enabled=1
        ;;
    wifi+rs485|rs485+wifi)
        wifi_enabled=1
        rs485_enabled=1
        ;;
    *)
        echo "Modo invalido: $mode" >&2
        exit 2
        ;;
esac

query="wifi_enabled=${wifi_enabled}&ethernet_enabled=${ethernet_enabled}&rs485_enabled=${rs485_enabled}"

echo "Aplicando perfil de transporte: $mode"
echo "GET ${base_url}/api/network/config?${query}"
curl --fail --silent --show-error "${base_url}/api/network/config?${query}"
echo

if [[ "$reboot_flag" == "--reboot" ]]; then
    echo "Reiniciando o no para aplicar a combinacao salva"
    curl --fail --silent --show-error "${base_url}/api/reboot"
    echo
else
    echo "Perfil salvo. Reinicie o no para aplicar a combinacao."
fi

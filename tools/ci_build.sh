#!/bin/bash
set -e

echo "=== Iniciando CI Build (ENDAP v1) ==="

# Verifica se o ambiente do ESP-IDF está carregado
if [ -z "$IDF_PATH" ]; then
    echo "Ambiente ESP-IDF não detectado. Tentando carregar de \$HOME/esp/esp-idf/export.sh..."
    if [ -f "$HOME/esp/esp-idf/export.sh" ]; then
        source "$HOME/esp/esp-idf/export.sh"
    else
        echo "ERRO: IDF_PATH não definido e export.sh não encontrado."
        echo "Por favor, ative o ambiente do ESP-IDF antes de rodar este script."
        exit 1
    fi
fi

echo ">> Passo 1: Limpeza do build (Clean)"
# Não apaga tudo agressivamente, apenas o idf.py fullclean ou build clean para garantir integridade
idf.py fullclean || true

echo ">> Passo 2: Compilando Firmware (Build)"
idf.py build

echo ">> Passo 3: Avaliando Tamanho do Binário"
idf.py size

# Passo Opcional: Verificação de impacto no kernel (apenas um aviso heurístico simples)
echo ">> Passo 4: Verificação de arquivos modificados (Kernel Audit)"
if git diff --quiet HEAD -- components/kernel/; then
    echo "OK: Nenhum arquivo em components/kernel/ foi modificado nesta branch."
else
    echo "AVISO: Modificações detectadas no Kernel (components/kernel/)."
    echo "Lembre-se: O Kernel v1 está congelado. Qualquer alteração deve ser justificada e auditada para garantir latência de 1ms."
fi

echo "=== CI Build Finalizado com Sucesso (EXIT CODE 0) ==="

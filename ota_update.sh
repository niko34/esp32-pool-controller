#!/bin/bash
# ota_update.sh - Mise à jour rapide sans recompilation
# Usage: ./ota_update.sh [firmware|filesystem|both] [hostname] [password]
#
# Authentification (par ordre de priorité) :
#   1. Argument $3 : ./ota_update.sh both poolcontroller.local monmotdepasse
#   2. Variable d'environnement POOL_PASSWORD : POOL_PASSWORD=monmotdepasse ./ota_update.sh
#   3. Variable d'environnement POOL_TOKEN    : POOL_TOKEN=<api_token> ./ota_update.sh
#   4. Saisie interactive si aucun des précédents n'est défini

set -e

TYPE="${1:-both}"
ESP32_HOST="${2:-poolcontroller.local}"
UPDATE_URL="http://$ESP32_HOST/update"

FIRMWARE_PATH=".pio/build/esp32dev/firmware.bin"
FILESYSTEM_PATH=".pio/build/esp32dev/littlefs.bin"

# Couleurs
GREEN='\033[0;32m'
BLUE='\033[0;34m'
RED='\033[0;31m'
NC='\033[0m'

# Résoudre les credentials
AUTH_ARGS=()
if [ -n "${3}" ]; then
    AUTH_ARGS=(-u "admin:${3}")
elif [ -n "${POOL_PASSWORD}" ]; then
    AUTH_ARGS=(-u "admin:${POOL_PASSWORD}")
elif [ -n "${POOL_TOKEN}" ]; then
    AUTH_ARGS=(-H "X-Auth-Token: ${POOL_TOKEN}")
else
    read -rsp "Mot de passe admin (poolcontroller.local) : " password
    echo
    AUTH_ARGS=(-u "admin:${password}")
fi

update_file() {
    local file=$1
    local name=$2
    local type=${3:-firmware}  # "firmware" ou "filesystem"

    if [ ! -f "$file" ]; then
        echo -e "${RED}❌ Fichier introuvable : $file${NC}"
        return 1
    fi

    echo -e "${BLUE}📤 Envoi de $name...${NC}"
    response=$(curl -s -w "\n%{http_code}" -X POST "${AUTH_ARGS[@]}" -F "update_type=$type" -F "update=@$file" "$UPDATE_URL")
    http_code=$(echo "$response" | tail -1)
    body=$(echo "$response" | head -1)

    if [ "$http_code" = "200" ] && echo "$body" | grep -q "OK"; then
        echo -e "${GREEN}✅ $name mis à jour${NC}"
        return 0
    elif [ "$http_code" = "401" ]; then
        echo -e "${RED}❌ Authentification refusée (mot de passe incorrect ?)${NC}"
        return 1
    else
        echo -e "${RED}❌ Erreur lors de la mise à jour de $name (HTTP $http_code : $body)${NC}"
        return 1
    fi
}

case "$TYPE" in
    firmware)
        update_file "$FIRMWARE_PATH" "Firmware" "firmware" || exit 1
        ;;
    filesystem|fs)
        update_file "$FILESYSTEM_PATH" "Filesystem" "filesystem" || exit 1
        ;;
    both)
        update_file "$FIRMWARE_PATH" "Firmware" "firmware" || exit 1
        sleep 30
        update_file "$FILESYSTEM_PATH" "Filesystem" "filesystem" || exit 1
        ;;
    *)
        echo "Usage: $0 [firmware|filesystem|both] [hostname] [password]"
        exit 1
        ;;
esac

echo -e "${GREEN}✅ Terminé !${NC}"

#!/bin/bash
# ota_update.sh - Mise à jour rapide sans recompilation
# Usage: ./ota_update.sh [firmware|filesystem|both] [hostname] [password]
#
# Le paramètre [hostname] accepte :
#   - Un nom mDNS : poolcontroller.local  (nécessite que le mDNS fonctionne sur le réseau)
#   - Une adresse IP : 192.168.1.42       (fonctionne sans mDNS, utile si le réseau ne le supporte pas)
# Par défaut : poolcontroller.local
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

# Environnement déployé : esp32dev_core3, seul env matériel depuis v2.42.1
# (ADR-0069 ; core 3 par défaut depuis v2.20.4, ADR-0042). Surchargeable par
#   PIO_ENV=<env> ./ota_update.sh firmware <hôte>
# Chemins surchargeables individuellement si besoin (FIRMWARE_PATH / FILESYSTEM_PATH).
#
# RAPPEL UTILE : l'OTA n'écrit QUE l'application, jamais le bootloader.
# Corollaire : tout changement de bootloader exige un flash USB (./deploy.sh),
# l'OTA n'y suffit pas. Le filesystem, lui, est indépendant du bootloader.
PIO_ENV="${PIO_ENV:-esp32dev_core3}"
FIRMWARE_PATH="${FIRMWARE_PATH:-.pio/build/$PIO_ENV/firmware.bin}"
FILESYSTEM_PATH="${FILESYSTEM_PATH:-.pio/build/$PIO_ENV/littlefs.bin}"

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
    if ! response=$(curl -s --show-error --connect-timeout 10 --max-time 120 \
            -w "\n%{http_code}" -X POST "${AUTH_ARGS[@]}" \
            -F "update_type=$type" -F "update=@$file" "$UPDATE_URL" 2>&1); then
        echo -e "${RED}❌ curl a échoué pour $name :${NC}"
        echo "$response"
        return 1
    fi
    http_code=$(echo "$response" | tail -1)
    body=$(echo "$response" | sed '$d')

    # Test STRICT : HTTP 200 ET corps exactement "OK".
    # L'ancien `grep -q "OK"` n'était pas ancré : depuis 2.20.1 le corps d'échec
    # est une phrase française "FAIL: <cause>" qui peut contenir "OK" (ex. le
    # nom du fichier envoyé) → faux succès silencieux.
    body_trimmed=$(printf '%s' "$body" | tr -d '[:space:]')

    if [ "$http_code" = "200" ] && [ "$body_trimmed" = "OK" ]; then
        echo -e "${GREEN}✅ $name mis à jour${NC}"
        return 0
    elif [ "$http_code" = "401" ]; then
        echo -e "${RED}❌ Authentification refusée (mot de passe incorrect ?)${NC}"
        return 1
    else
        echo -e "${RED}❌ Échec de la mise à jour de $name (HTTP $http_code)${NC}"
        if [ -n "$body" ]; then
            echo -e "${RED}   Réponse du contrôleur : $body${NC}"
        else
            echo -e "${RED}   Réponse vide — consulter le Journal du contrôleur${NC}"
        fi
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

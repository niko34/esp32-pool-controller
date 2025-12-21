#!/bin/bash
# quick_update.sh - Mise à jour rapide sans recompilation
# Usage: ./quick_update.sh [firmware|filesystem|both] [hostname]

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

update_file() {
    local file=$1
    local name=$2

    if [ ! -f "$file" ]; then
        echo -e "${RED}❌ Fichier introuvable : $file${NC}"
        return 1
    fi

    echo -e "${BLUE}📤 Envoi de $name...${NC}"
    if curl -X POST -F "update=@$file" "$UPDATE_URL" 2>&1 | grep -q "OK"; then
        echo -e "${GREEN}✅ $name mis à jour${NC}"
        return 0
    else
        echo -e "${RED}❌ Erreur lors de la mise à jour de $name${NC}"
        return 1
    fi
}

case "$TYPE" in
    firmware)
        update_file "$FIRMWARE_PATH" "Firmware"
        ;;
    filesystem|fs)
        update_file "$FILESYSTEM_PATH" "Filesystem"
        ;;
    both)
        update_file "$FIRMWARE_PATH" "Firmware" && sleep 30 && update_file "$FILESYSTEM_PATH" "Filesystem"
        ;;
    *)
        echo "Usage: $0 [firmware|filesystem|both] [hostname]"
        exit 1
        ;;
esac

echo -e "${GREEN}✅ Terminé !${NC}"

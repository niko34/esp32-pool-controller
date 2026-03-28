#!/bin/bash
# Script de déploiement complet pour ESP32 Pool Controller
# Usage: ./deploy.sh [firmware|fs|all|ota-firmware|ota-fs|ota-all]

set -e  # Arrêter en cas d'erreur

# Configuration
PORT="/dev/cu.usbserial-0001"
BAUD="115200"
LITTLEFS_OFFSET="0x2B0000"
BUILD_DIR=".pio/build/esp32dev"

# Couleurs pour l'affichage
GREEN='\033[0;32m'
BLUE='\033[0;34m'
RED='\033[0;31m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

print_step() {
    echo -e "${BLUE}==>${NC} $1"
}

print_success() {
    echo -e "${GREEN}✅${NC} $1"
}

print_error() {
    echo -e "${RED}❌${NC} $1"
}

print_warning() {
    echo -e "${YELLOW}⚠️${NC} $1"
}

build_firmware() {
    print_step "Compilation du firmware..."
    pio run
    print_success "Firmware compilé"
}

build_filesystem() {
    print_step "Construction du filesystem LittleFS (1344KB)..."
    ./build_fs.sh
}

upload_firmware() {
    print_step "Upload du firmware..."
    pio run -t upload
    # Effacer l'OTA data pour forcer le boot sur app0 (partition table à jour)
    print_step "Réinitialisation OTA data (boot → app0)..."
    ~/.platformio/penv/bin/python ~/.platformio/packages/tool-esptoolpy/esptool.py \
        --chip esp32 \
        --port "$PORT" \
        --baud "$BAUD" \
        erase_region 0xE000 0x2000
    print_success "Firmware uploadé"
}

upload_filesystem() {
    print_step "Upload du filesystem LittleFS..."

    if [ ! -f "$BUILD_DIR/littlefs.bin" ]; then
        print_error "Le fichier littlefs.bin n'existe pas. Exécutez d'abord ./build_fs.sh"
        exit 1
    fi

    # Vérifier la taille du fichier
    SIZE=$(stat -f%z "$BUILD_DIR/littlefs.bin" 2>/dev/null || stat -c%s "$BUILD_DIR/littlefs.bin" 2>/dev/null)
    if [ "$SIZE" != "1245184" ]; then
        print_warning "Taille incorrecte du fichier littlefs.bin: $SIZE bytes (attendu: 1245184)"
        print_warning "Reconstruction avec build_fs.sh..."
        build_filesystem
    fi

    ~/.platformio/penv/bin/python ~/.platformio/packages/tool-esptoolpy/esptool.py \
        --chip esp32 \
        --port "$PORT" \
        --baud "$BAUD" \
        write_flash "$LITTLEFS_OFFSET" "$BUILD_DIR/littlefs.bin"

    print_success "Filesystem uploadé"
}

show_usage() {
    echo "Usage: $0 [firmware|fs|all|ota-firmware|ota-fs|ota-all]"
    echo ""
    echo "Options USB (connexion série requise) :"
    echo "  firmware      - Compile et upload uniquement le firmware"
    echo "  fs            - Build et upload uniquement le filesystem"
    echo "  all           - Compile et upload firmware + filesystem (défaut)"
    echo ""
    echo "Options OTA (WiFi, pas d'USB) :"
    echo "  ota-firmware  - Compile et envoie uniquement le firmware en OTA"
    echo "  ota-fs        - Build et envoie uniquement le filesystem en OTA"
    echo "  ota-all       - Compile et envoie firmware + filesystem en OTA"
    echo ""
    echo "Exemples:"
    echo "  $0                # Déploiement USB complet"
    echo "  $0 ota-all        # Déploiement OTA complet (WiFi)"
    echo "  $0 ota-firmware   # Firmware uniquement en OTA (Wifi)"
}

# Programme principal
MODE="${1:-all}"

case "$MODE" in
    firmware)
        print_step "Déploiement USB du firmware uniquement"
        build_firmware
        upload_firmware
        ;;
    fs)
        print_step "Déploiement USB du filesystem uniquement"
        build_filesystem
        upload_filesystem
        ;;
    all)
        print_step "Déploiement USB complet (firmware + filesystem)"
        build_firmware
        build_filesystem
        upload_firmware
        upload_filesystem
        ;;
    ota-firmware)
        print_step "Déploiement OTA du firmware uniquement"
        build_firmware
        ./ota_update.sh firmware
        ;;
    ota-fs)
        print_step "Déploiement OTA du filesystem uniquement"
        build_filesystem
        ./ota_update.sh filesystem
        ;;
    ota-all)
        print_step "Déploiement OTA complet (firmware + filesystem)"
        build_firmware
        build_filesystem
        ./ota_update.sh both
        ;;
    -h|--help)
        show_usage
        exit 0
        ;;
    *)
        print_error "Option invalide: $MODE"
        show_usage
        exit 1
        ;;
esac

echo ""
print_success "Déploiement terminé avec succès!"
